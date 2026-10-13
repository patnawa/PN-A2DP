/*
 * driver.cpp - DriverEntry, PortCls adapter glue, dispatch hooks.
 *
 * The driver is a PortCls adapter driver (audio endpoint) that also behaves as a
 * Bluetooth profile driver. KMDF is used in miniport mode for I/O targets only.
 *
 * SPDX-License-Identifier: MIT
 */
#define PN_DEFINE_GUIDS
#include "pna2dp.h"

/* ---- C++ runtime support for kernel mode ------------------------------------ */

void *__cdecl operator new(size_t Size, POOL_FLAGS PoolFlags, ULONG Tag)
{
    if (Size == 0) Size = 1;
    /* never executable pool, whatever a caller passes; /analyze cannot see through the
     * mask on a variable POOL_FLAGS, hence the suppression */
#pragma warning(suppress: 28160)
    void *p = ExAllocatePool2(PoolFlags & ~POOL_FLAG_NON_PAGED_EXECUTE, Size, Tag);
    return p;
}
void *__cdecl operator new(size_t Size, POOL_FLAGS PoolFlags)
{
    return operator new(Size, PoolFlags, PNA2DP_TAG_GENERIC);
}
void __cdecl operator delete(void *p, POOL_FLAGS, ULONG)
{
    if (p) ExFreePool(p);
}
/* Sized delete (C++14), used by "delete this" in the COM objects. The unsized global
 * operator delete(void *) comes from stdunk.lib and also calls ExFreePool. */
void __cdecl operator delete(void *p, size_t) noexcept
{
    if (p) ExFreePool(p);
}

/* Pure virtual call handler required by the C++ ABI: a corrupted or half-destroyed object.
 * Fail fast (bugcheck 0x139 KERNEL_SECURITY_CHECK_FAILURE) instead of a private code. */
extern "C" int __cdecl _purecall(void)
{
    __fastfail(FAST_FAIL_INVALID_ARG);
    return 0;   /* not reached */
}

/* ---- globals ------------------------------------------------------------------- */

typedef void (*PFN_PC_UNLOAD)(PDRIVER_OBJECT);
static PFN_PC_UNLOAD g_PcUnload = NULL;
static PDRIVER_DISPATCH g_PcPnpDispatch = NULL;
static PDRIVER_DISPATCH g_PcShutdown = NULL;     /* NULL when PortCls does not handle IRP_MJ_SHUTDOWN */
static PDRIVER_DISPATCH g_PortDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];

NTSTATUS PnPortDispatchIrp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UCHAR major = IoGetCurrentIrpStackLocation(Irp)->MajorFunction;
    if (major == IRP_MJ_CREATE) PN_VERB("KS create %wZ", &IoGetCurrentIrpStackLocation(Irp)->FileObject->FileName);
    NTSTATUS status = g_PortDispatch[major](DeviceObject, Irp);
    // The original handler owns/completes the IRP; do not touch it after dispatch.
    if (major == IRP_MJ_CREATE && !NT_SUCCESS(status)) PN_WARN("PortCls create failed 0x%08x", status);
    return status;
}

/* Crash guard: a system thread clears the unclean-start count after 10 minutes of uptime. */
static PDRIVER_OBJECT g_DriverObject = NULL;
static KEVENT g_GuardStop;
static PKTHREAD g_GuardThread = NULL;
extern "C" NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(_In_ HANDLE Handle, _In_ BOOLEAN Alertable, _In_opt_ PLARGE_INTEGER Timeout);

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" DRIVER_UNLOAD PnDriverUnload;
static DRIVER_ADD_DEVICE PnAddDevice;
_Dispatch_type_(IRP_MJ_PNP) static DRIVER_DISPATCH PnPnpHandler;
_Dispatch_type_(IRP_MJ_SHUTDOWN) static DRIVER_DISPATCH PnShutdownHandler;
static KSTART_ROUTINE PnGuardThread;
static VOID PnStopGuardThread(void);
static NTSTATUS PnStartDevice(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PRESOURCELIST ResourceList);

/* ---- entry ---------------------------------------------------------------------- */

#pragma code_seg("INIT")
extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags |= WdfDriverInitNoDispatchOverride;
    config.DriverPoolTag = PNA2DP_TAG_GENERIC;
    /* Tracing first: every failure below is traced and written to the System event log,
     * and each failure path unregisters the ETW provider again. */
    PnTraceInitialize(DriverObject);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        PN_ERR("WdfDriverCreate failed 0x%08x", status);
        PnLogDeviceEvent(NULL, PN_EVENT_LOAD_FAILED, (ULONG)status);
        PnTraceUninitialize();
        return status;
    }

    DeviceListInitialize();
    status = RegCfgInitialize(DriverObject);
    if (!NT_SUCCESS(status)) {
        PN_ERR("RegCfgInitialize failed 0x%08x", status);
        PnLogDeviceEvent(NULL, PN_EVENT_LOAD_FAILED, (ULONG)status);
        WdfDriverMiniportUnload(WdfGetDriver());
        PnTraceUninitialize();
        return status;
    }
    RegCfgLoadGlobal(DriverObject);
    PN_INFO("pna2dp %u.%u.%u loading", PNA2DP_DRIVER_VERSION >> 24, (PNA2DP_DRIVER_VERSION >> 16) & 0xFF, PNA2DP_DRIVER_VERSION & 0xFFFF);

    ULONG unclean = 0;
    if (!RegCfgCrashGuardEnter(DriverObject, &unclean)) {
        PN_ERR("crash guard: the last %u starts ended without a clean shutdown; not loading. "
               "Delete HKLM\\SYSTEM\\CurrentControlSet\\Services\\pna2dp\\State\\UncleanStarts to try again.", unclean);
        PnLogDeviceEvent(NULL, PN_EVENT_CRASH_GUARD, unclean);
        RegCfgShutdown();
        WdfDriverMiniportUnload(WdfGetDriver());
        PnTraceUninitialize();
        return STATUS_UNSUCCESSFUL;
    }
    g_DriverObject = DriverObject;
    KeInitializeEvent(&g_GuardStop, NotificationEvent, FALSE);
    {
        HANDLE h = NULL;
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, PnGuardThread, NULL))) {
            if (!NT_SUCCESS(ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&g_GuardThread, NULL))) {
                // A created thread must not outlive this driver's code if the reference fails.
                g_GuardThread = NULL;
                KeSetEvent(&g_GuardStop, 0, FALSE);
                ZwWaitForSingleObject(h, FALSE, NULL);
            }
            ZwClose(h);
        }
    }

    status = PcInitializeAdapterDriver(DriverObject, RegistryPath, (PDRIVER_ADD_DEVICE)PnAddDevice);
    if (!NT_SUCCESS(status)) {
        PN_ERR("PcInitializeAdapterDriver failed 0x%08x", status);
        PnLogDeviceEvent(NULL, PN_EVENT_LOAD_FAILED, (ULONG)status);
        PnStopGuardThread();
        RegCfgShutdown();
        WdfDriverMiniportUnload(WdfGetDriver());
        PnTraceUninitialize();
        return status;
    }

    /* hook what we need; everything else stays with PortCls */
    RtlCopyMemory(g_PortDispatch, DriverObject->MajorFunction, sizeof(g_PortDispatch));
    g_PcPnpDispatch = DriverObject->MajorFunction[IRP_MJ_PNP];
    DriverObject->MajorFunction[IRP_MJ_PNP] = PnPnpHandler;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = ControlDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = ControlDispatchCleanup;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = ControlDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ControlDispatchDeviceControl;
    /* Every other major PortCls installed sees control handles first: KS must never take a
     * control handle's FsContext for its object header (review round 4 #6). PNP, POWER and
     * SYSTEM_CONTROL carry no control file object; a slot PortCls left at the I/O manager
     * default (that of IRP_MJ_CREATE_MAILSLOT, below) already fails the request. */
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        if (i == IRP_MJ_PNP || i == IRP_MJ_CREATE || i == IRP_MJ_CLEANUP || i == IRP_MJ_CLOSE || i == IRP_MJ_DEVICE_CONTROL ||
            i == IRP_MJ_SHUTDOWN || i == IRP_MJ_POWER || i == IRP_MJ_SYSTEM_CONTROL) continue;
        if (DriverObject->MajorFunction[i] == DriverObject->MajorFunction[IRP_MJ_CREATE_MAILSLOT]) continue;
        DriverObject->MajorFunction[i] = ControlDispatchOther;
    }
    /* IRP_MJ_CREATE_MAILSLOT is never set by PortCls, so it holds the I/O manager default */
    if (DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] != DriverObject->MajorFunction[IRP_MJ_CREATE_MAILSLOT]) g_PcShutdown = DriverObject->MajorFunction[IRP_MJ_SHUTDOWN];
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = PnShutdownHandler;
    g_PcUnload = (PFN_PC_UNLOAD)DriverObject->DriverUnload;
    DriverObject->DriverUnload = PnDriverUnload;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
static VOID PnGuardThread(_In_ PVOID Context)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    LARGE_INTEGER t;
    t.QuadPart = -PN_HNS_PER_SEC * 600;
    if (KeWaitForSingleObject(&g_GuardStop, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT && g_DriverObject) {
        RegCfgCrashGuardClear(g_DriverObject);
        PN_INFO("crash guard: 10 minutes up, start counted as clean");
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

#pragma code_seg("PAGE")
static VOID PnStopGuardThread(void)
{
    PAGED_CODE();
    if (!g_GuardThread) return;
    KeSetEvent(&g_GuardStop, 0, FALSE);
    KeWaitForSingleObject(g_GuardThread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(g_GuardThread);
    g_GuardThread = NULL;
}

/* Registered per FDO in PnStartDevice: a clean shutdown or restart resets the crash guard. */
#pragma code_seg("PAGE")
static NTSTATUS PnShutdownHandler(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();
    RegCfgCrashGuardClear(DeviceObject->DriverObject);
    if (g_PcShutdown) return g_PcShutdown(DeviceObject, Irp);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
extern "C" VOID PnDriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    PN_INFO("unloading");
    PnStopGuardThread();
    RegCfgCrashGuardClear(DriverObject);   /* every device was removed cleanly */
    if (g_PcUnload) g_PcUnload(DriverObject);
    RegCfgShutdown();
    if (WdfGetDriver()) WdfDriverMiniportUnload(WdfGetDriver());
    PnTraceUninitialize();
}

#pragma code_seg("PAGE")
static NTSTATUS PnAddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PAGED_CODE();
    /* two subdevices (wave + topology) */
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, PCPFNSTARTDEVICE(PnStartDevice), 2, 0);
}

typedef struct _PN_PORTCLS_EXTENSION {
    ULONG_PTR       Reserved1[2];
    PDEVICE_OBJECT  Pdo;
    PVOID           Reserved2;
    PVOID           Reserved3;
    IPnAdapterCommon *Common;    /* same slot layout as the Microsoft samples (m_pCommon) */
    PVOID           Unused1;
    PVOID           Unused2;
} PN_PORTCLS_EXTENSION, *PPN_PORTCLS_EXTENSION;

#pragma code_seg("PAGE")
static NTSTATUS PnStartDevice(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PRESOURCELIST ResourceList)
{
    PAGED_CODE();
    NTSTATUS status;
    PUNKNOWN unknown = NULL;
    IPnAdapterCommon *common = NULL;
    PPN_PORTCLS_EXTENSION ext = (PPN_PORTCLS_EXTENSION)DeviceObject->DeviceExtension;

    status = NewPnAdapterCommon(&unknown, GUID_NULL, NULL, POOL_FLAG_NON_PAGED);
    if (NT_SUCCESS(status)) {
        status = unknown->QueryInterface(IID_IPnAdapterCommon, (PVOID *)&common);
        unknown->Release();
    }
    if (!NT_SUCCESS(status)) {
        PN_ERR("StartDevice: adapter object failed 0x%08x", status);
        PnLogDeviceEvent(DeviceObject, PN_EVENT_START_FAILED, (ULONG)status);
        return status;
    }

    status = common->Init(DeviceObject, DeviceObject->DriverObject);
    if (NT_SUCCESS(status)) status = PcRegisterAdapterPowerManagement(PUNKNOWN(common), DeviceObject);
    if (NT_SUCCESS(status)) status = common->InstallFilters(Irp, ResourceList);
    if (!NT_SUCCESS(status)) {
        PN_ERR("StartDevice failed 0x%08x", status);
        PnLogDeviceEvent(DeviceObject, PN_EVENT_START_FAILED, (ULONG)status);
        common->Cleanup();
        common->Release();
        return status;
    }
    ext->Common = common;
    if (!NT_SUCCESS(IoRegisterShutdownNotification(DeviceObject))) PN_WARN("no shutdown notification; crash guard relies on the uptime timer");
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
static NTSTATUS PnPnpHandler(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (sp->MinorFunction == IRP_MN_REMOVE_DEVICE || sp->MinorFunction == IRP_MN_SURPRISE_REMOVAL || sp->MinorFunction == IRP_MN_STOP_DEVICE) {
        PPN_PORTCLS_EXTENSION ext = (PPN_PORTCLS_EXTENSION)DeviceObject->DeviceExtension;
        IPnAdapterCommon *common = ext ? ext->Common : NULL;
        if (common) {
            PN_INFO("PnP minor 0x%02x: tearing down", sp->MinorFunction);
            IoUnregisterShutdownNotification(DeviceObject);
            ext->Common = NULL;
            common->Cleanup();
            common->Release();
        }
    }
    return PcDispatchIrp(DeviceObject, Irp);
}
#pragma code_seg()
