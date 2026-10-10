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
    void *p = ExAllocatePool2(PoolFlags, Size, Tag);
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
void __cdecl operator delete(void *p, POOL_FLAGS)
{
    if (p) ExFreePool(p);
}

/* pure virtual call handler required by the C++ ABI */
extern "C" int __cdecl _purecall(void)
{
    KeBugCheckEx(0xDEADC0DE, 0, 0, 0, 0);
}

/* ---- globals ------------------------------------------------------------------- */

typedef void (*PFN_PC_UNLOAD)(PDRIVER_OBJECT);
static PFN_PC_UNLOAD g_PcUnload = NULL;
static PDRIVER_DISPATCH g_PcPnpDispatch = NULL;

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" DRIVER_UNLOAD PnDriverUnload;
static DRIVER_ADD_DEVICE PnAddDevice;
_Dispatch_type_(IRP_MJ_PNP) static DRIVER_DISPATCH PnPnpHandler;
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
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;

    RegCfgLoadGlobal(DriverObject);
    PN_INFO("pna2dp %u.%u.%u loading", PNA2DP_DRIVER_VERSION >> 24, (PNA2DP_DRIVER_VERSION >> 16) & 0xFF, PNA2DP_DRIVER_VERSION & 0xFFFF);

    status = PcInitializeAdapterDriver(DriverObject, RegistryPath, (PDRIVER_ADD_DEVICE)PnAddDevice);
    if (!NT_SUCCESS(status)) {
        PN_ERR("PcInitializeAdapterDriver failed 0x%08x", status);
        WdfDriverMiniportUnload(WdfGetDriver());
        return status;
    }

    /* hook what we need; everything else stays with PortCls */
    g_PcPnpDispatch = DriverObject->MajorFunction[IRP_MJ_PNP];
    DriverObject->MajorFunction[IRP_MJ_PNP] = PnPnpHandler;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = ControlDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = ControlDispatchCleanup;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = ControlDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ControlDispatchDeviceControl;
    g_PcUnload = (PFN_PC_UNLOAD)DriverObject->DriverUnload;
    DriverObject->DriverUnload = PnDriverUnload;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
extern "C" VOID PnDriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    PN_INFO("unloading");
    if (g_PcUnload) g_PcUnload(DriverObject);
    if (WdfGetDriver()) WdfDriverMiniportUnload(WdfGetDriver());
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
    UNREFERENCED_PARAMETER(ResourceList);
    NTSTATUS status;
    PUNKNOWN unknown = NULL;
    IPnAdapterCommon *common = NULL;
    PPN_PORTCLS_EXTENSION ext = (PPN_PORTCLS_EXTENSION)DeviceObject->DeviceExtension;

    status = NewPnAdapterCommon(&unknown, GUID_NULL, NULL, POOL_FLAG_NON_PAGED);
    if (!NT_SUCCESS(status)) return status;
    status = unknown->QueryInterface(IID_IPnAdapterCommon, (PVOID *)&common);
    unknown->Release();
    if (!NT_SUCCESS(status)) return status;

    status = common->Init(DeviceObject, DeviceObject->DriverObject);
    if (NT_SUCCESS(status)) status = PcRegisterAdapterPowerManagement(PUNKNOWN(common), DeviceObject);
    if (NT_SUCCESS(status)) status = common->InstallFilters(Irp);
    if (!NT_SUCCESS(status)) {
        PN_ERR("StartDevice failed 0x%08x", status);
        common->Cleanup();
        common->Release();
        return status;
    }
    ext->Common = common;
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
            ext->Common = NULL;
            common->Cleanup();
            common->Release();
        }
    }
    return PcDispatchIrp(DeviceObject, Irp);
}
#pragma code_seg()
