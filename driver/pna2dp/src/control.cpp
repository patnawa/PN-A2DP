/*
 * control.cpp - private control interface dispatch (create/cleanup/close/ioctl).
 * SPDX-License-Identifier: MIT
 */
#include <ntifs.h>      /* token queries for the per-handle caller role */
#include "pna2dp.h"
#include "control_access.h"

/* FsContext2 of a control handle: 'PAC' tag with the caller's PN_CTL_ROLE_* in the low byte.
 * No kernel pointer (KS object headers) can take one of these values. */
#define PN_CTL_FILE_MAGIC   0x50414300u
#define PN_CTL_FILE_TAG(role) ((PVOID)(ULONG_PTR)(PN_CTL_FILE_MAGIC | (role)))

BOOLEAN ControlIsOurFile(_In_ PFILE_OBJECT File)
{
    if (!File) return FALSE;
    ULONG_PTR v = (ULONG_PTR)File->FsContext2;
    return (v & ~(ULONG_PTR)0xFF) == PN_CTL_FILE_MAGIC && (v & 0xFF) <= PN_CTL_ROLE_SYSTEM;
}

static ULONG ControlFileRole(_In_ PFILE_OBJECT File)
{
    return (ULONG)((ULONG_PTR)File->FsContext2 & 0xFF);
}

/* S-1-5-18 and S-1-5-32-544, laid out as SIDs (SID declares one sub-authority). */
static const SID g_PnLocalSystemSid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_LOCAL_SYSTEM_RID } };
static const struct { UCHAR Revision, SubAuthorityCount; SID_IDENTIFIER_AUTHORITY IdentifierAuthority; ULONG SubAuthority[2]; }
    g_PnAdminsSid = { SID_REVISION, 2, SECURITY_NT_AUTHORITY, { SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS } };

/* Role of the token that opens a control handle (the impersonation token when the caller
 * impersonates). Any query failure yields the least-privileged role. */
static ULONG ControlCallerRole(_In_ PIO_STACK_LOCATION Sp)
{
    PAGED_CODE();
    PIO_SECURITY_CONTEXT sc = Sp->Parameters.Create.SecurityContext;
    if (!sc || !sc->AccessState) return PN_CTL_ROLE_USER;
    PSECURITY_SUBJECT_CONTEXT subject = &sc->AccessState->SubjectSecurityContext;
    BOOLEAN system = FALSE, admins = FALSE;
    ULONG adminsAttributes = 0;
    SeLockSubjectContext(subject);
    PACCESS_TOKEN token = SeQuerySubjectContextToken(subject);
    if (token) {
        PTOKEN_USER user = NULL;
        if (NT_SUCCESS(SeQueryInformationToken(token, TokenUser, (PVOID *)&user)) && user) {
            system = RtlEqualSid(user->User.Sid, (PSID)&g_PnLocalSystemSid);
            ExFreePool(user);
        }
        PTOKEN_GROUPS groups = NULL;
        if (!system && NT_SUCCESS(SeQueryInformationToken(token, TokenGroups, (PVOID *)&groups)) && groups) {
            for (ULONG i = 0; i < groups->GroupCount; ++i) {
                if (RtlEqualSid(groups->Groups[i].Sid, (PSID)&g_PnAdminsSid)) {
                    admins = TRUE;
                    adminsAttributes = groups->Groups[i].Attributes;
                    break;
                }
            }
            ExFreePool(groups);
        }
    }
    SeUnlockSubjectContext(subject);
    return PnControlRoleFromToken(system, admins, adminsAttributes);
}

NTSTATUS ControlRegisterInterface(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    UNICODE_STRING ref;
    RtlInitUnicodeString(&ref, PNA2DP_CONTROL_REFSTRING);
    NTSTATUS status = IoRegisterDeviceInterface(D->Pdo, &GUID_DEVINTERFACE_PNA2DP, &ref, &D->InterfaceSymlink);
    if (!NT_SUCCESS(status)) { PN_WARN("IoRegisterDeviceInterface failed 0x%08x", status); return status; }
    status = IoSetDeviceInterfaceState(&D->InterfaceSymlink, TRUE);
    if (!NT_SUCCESS(status)) { PN_WARN("IoSetDeviceInterfaceState failed 0x%08x", status); RtlFreeUnicodeString(&D->InterfaceSymlink); return status; }
    D->InterfaceRegistered = TRUE;
    PN_INFO("control interface %wZ", &D->InterfaceSymlink);
    return STATUS_SUCCESS;
}

VOID ControlUnregisterInterface(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    if (D->InterfaceRegistered) {
        NTSTATUS status = IoSetDeviceInterfaceState(&D->InterfaceSymlink, FALSE);
        if (!NT_SUCCESS(status)) PN_WARN("IoSetDeviceInterfaceState(FALSE) failed 0x%08x", status);
        RtlFreeUnicodeString(&D->InterfaceSymlink);
        D->InterfaceRegistered = FALSE;
    }
}

static BOOLEAN IsControlOpen(_In_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT f = sp->FileObject;
    if (!f) return FALSE;
    /* FileName is "\pna2dpctl" when opened through the device interface with our reference string */
    UNICODE_STRING ref;
    RtlInitUnicodeString(&ref, L"\\" PNA2DP_CONTROL_REFSTRING);
    return RtlEqualUnicodeString(&f->FileName, &ref, TRUE);
}

static NTSTATUS CompleteIrp(_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Info)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* Each control handle holds a reference on the device context (released at CLOSE), so the
 * context outlives the handle even after a surprise removal. IOCTLs and CLEANUP run under
 * the device's control rundown, which DeviceStop waits for before it drains the queues. */
NTSTATUS ControlDispatchCreate(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();
    if (!IsControlOpen(Irp)) return PnPortDispatchIrp(DeviceObject, Irp);
    PPNA2DP_DEVICE D = DeviceReferenceFromFdo(DeviceObject);
    if (!D) return CompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG role = ControlCallerRole(sp);
    sp->FileObject->FsContext2 = PN_CTL_FILE_TAG(role);
    sp->FileObject->FsContext = D;
    PN_VERB("control handle opened by pid %u, role %u", IoGetRequestorProcessId(Irp), role);
    InterlockedIncrement(&D->OpenHandles);
    return CompleteIrp(Irp, STATUS_SUCCESS, FILE_OPENED);
}

NTSTATUS ControlDispatchCleanup(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (!ControlIsOurFile(sp->FileObject)) return PnPortDispatchIrp(DeviceObject, Irp);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)sp->FileObject->FsContext;
    /* After DeviceStop the queues are already drained and the host unregistered. */
    if (D && D->Signature == PNA2DP_DEVICE_SIGNATURE && ExAcquireRundownProtection(&D->ControlRundown)) {
        DeviceFlushWaitersForFile(D, sp->FileObject);
        CodecHostFlushFile(&D->Host, sp->FileObject);
        ExReleaseRundownProtection(&D->ControlRundown);
    }
    return CompleteIrp(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS ControlDispatchClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (!ControlIsOurFile(sp->FileObject)) return PnPortDispatchIrp(DeviceObject, Irp);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)sp->FileObject->FsContext;
    sp->FileObject->FsContext2 = NULL;
    sp->FileObject->FsContext = NULL;
    if (D && D->Signature == PNA2DP_DEVICE_SIGNATURE) {
        InterlockedDecrement(&D->OpenHandles);
        DeviceRelease(D);   /* may free D */
    }
    return CompleteIrp(Irp, STATUS_SUCCESS, 0);
}

/* Every other major PortCls installed (READ, WRITE, FLUSH_BUFFERS, QUERY/SET_SECURITY ...;
 * driver.cpp): a control handle is no KS object, and KS would read its FsContext as an
 * object header and dispatch through it. Such requests on a control handle are invalid
 * (review round 4 #6); everything else goes to PortCls unchanged. Not paged. */
NTSTATUS ControlDispatchOther(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (!ControlIsOurFile(sp->FileObject)) return PnPortDispatchIrp(DeviceObject, Irp);
    return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS ControlDeviceControl(_In_ PPNA2DP_DEVICE D, _Inout_ PIRP Irp);

NTSTATUS ControlDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (!ControlIsOurFile(sp->FileObject)) return PnPortDispatchIrp(DeviceObject, Irp);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)sp->FileObject->FsContext;
    if (!D || D->Signature != PNA2DP_DEVICE_SIGNATURE || !ExAcquireRundownProtection(&D->ControlRundown)) {
        return CompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }
    NTSTATUS status = D->Removing ? CompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0) : ControlDeviceControl(D, Irp);
    ExReleaseRundownProtection(&D->ControlRundown);
    return status;
}

/* Runs under the control rundown. Pended IRPs are owned by a cancel-safe queue as soon as
 * IoCsqInsertIrp returns (it also marks them pending), so they must not be touched after. */
static NTSTATUS ControlDeviceControl(_In_ PPNA2DP_DEVICE D, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);

    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    if (METHOD_FROM_CTL_CODE(code) != METHOD_BUFFERED) return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    /* Caller role captured at open (see control_access.h for which role may do what). */
    if (!PnControlRoleAllows(ControlFileRole(sp->FileObject), code)) {
        PN_VERB("control request 0x%08x denied for role %u", code, ControlFileRole(sp->FileObject));
        return CompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
    }

    switch (code) {
    case IOCTL_PNA2DP_SBC_SAMPLE: {
        if (inLen != sizeof(ULONG)) { status = STATUS_INVALID_PARAMETER; break; }
        ULONG arm = *(ULONG *)buf;
        if (arm > 2) { status = STATUS_INVALID_PARAMETER; break; }
        if (!arm && outLen < sizeof(PNA2DP_SBC_SAMPLE)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = PumpSbcSample(&D->Pump, arm, arm ? NULL : (PNA2DP_SBC_SAMPLE *)buf);
        if (!arm && NT_SUCCESS(status)) info = sizeof(PNA2DP_SBC_SAMPLE);
        break;
    }
    case IOCTL_PNA2DP_GET_TRACE:
        if (outLen < sizeof(PNA2DP_TRACE)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        PnTraceRead((PNA2DP_TRACE *)buf);
        info = sizeof(PNA2DP_TRACE);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_PNA2DP_GET_VERSION: {
        if (outLen < sizeof(PNA2DP_VERSION)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        PNA2DP_VERSION *v = (PNA2DP_VERSION *)buf;
        RtlZeroMemory(v, sizeof(*v));
        v->AbiVersion = PNA2DP_ABI_VERSION;
        v->DriverVersion = PNA2DP_DRIVER_VERSION;
        v->Role = 1;
        v->KernelCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
        v->HostCodecMask = CodecHostCodecMask(&D->Host);
        info = sizeof(*v);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_PNA2DP_GET_STATUS:
        if (outLen < sizeof(PNA2DP_STATUS)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = DeviceGetStatus(D, (PPNA2DP_STATUS)buf);
        info = sizeof(PNA2DP_STATUS);
        break;
    case IOCTL_PNA2DP_GET_CAPABILITIES:
        if (outLen < sizeof(PNA2DP_CAPABILITIES)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = DeviceGetCapabilities(D, (PPNA2DP_CAPABILITIES)buf);
        info = sizeof(PNA2DP_CAPABILITIES);
        break;
    case IOCTL_PNA2DP_GET_CONFIG:
        if (outLen < sizeof(PNA2DP_CONFIG)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = DeviceGetConfig(D, (PPNA2DP_CONFIG)buf);
        info = sizeof(PNA2DP_CONFIG);
        break;
    case IOCTL_PNA2DP_SET_CONFIG:
        if (inLen < sizeof(PNA2DP_CONFIG)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = DeviceSetConfig(D, (const PNA2DP_CONFIG *)buf);
        break;
    case IOCTL_PNA2DP_WAIT_EVENT:
        status = DeviceQueueWaitEvent(D, Irp);
        if (status == STATUS_PENDING) return STATUS_PENDING;
        if (NT_SUCCESS(status)) info = Irp->IoStatus.Information;
        break;
    case IOCTL_PNA2DP_RECONNECT:
        status = DeviceReconnect(D);
        break;
    case IOCTL_PNA2DP_GET_STATS:
        if (outLen < sizeof(PNA2DP_STATS)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        PumpGetStats(&D->Pump, (PPNA2DP_STATS)buf);
        info = sizeof(PNA2DP_STATS);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_PNA2DP_GET_QUALITY: {
        if (outLen < sizeof(PNA2DP_QUALITY)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        PNA2DP_QUALITY *q = (PNA2DP_QUALITY *)buf;
        PumpGetQuality(&D->Pump, q);
        q->LocalAddress = D->Link.LocalAddress;
        q->RadioQueryStatus = D->Link.RadioQueryStatus;
        if (D->Link.RadioInfoValid) {
            const BTH_LOCAL_RADIO_INFO *r = &D->Link.RadioInfo;
            q->Flags |= PNA2DP_QUALITY_RADIO_VALID;
            q->LmpFeatures = r->radioInfo.lmpSupportedFeatures;
            q->Manufacturer = r->radioInfo.mfg;
            q->HciRevision = r->hciRevision; q->HciVersion = r->hciVersion;
            q->LmpSubversion = r->radioInfo.lmpSubversion; q->LmpVersion = r->radioInfo.lmpVersion;
        }
        PNA2DP_CONFIG cfg;
        DeviceGetConfig(D, &cfg);
        if (cfg.Flags & PNA2DP_CFG_AUTO_QUALITY) {
            q->Flags |= PNA2DP_QUALITY_AUTO;
            if (PnAdapterNeedsConservativeProfile(D->Link.RadioInfoValid, q->LmpFeatures)) q->Flags |= PNA2DP_QUALITY_CONSERVATIVE;
        }
        info = sizeof(*q); status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_PNA2DP_RESET_STATS:
        PumpResetStats(&D->Pump);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_PNA2DP_CODEC_REGISTER:
        if (inLen < sizeof(PNA2DP_CODEC_REGISTER)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        if (!PnControlCodecHostPidValid(((const PNA2DP_CODEC_REGISTER *)buf)->HostPid, IoGetRequestorProcessId(Irp))) {
            status = STATUS_ACCESS_DENIED;
            break;
        }
        status = CodecHostRegister(&D->Host, sp->FileObject, (const PNA2DP_CODEC_REGISTER *)buf);
        break;
    case IOCTL_PNA2DP_CODEC_UNREGISTER:
        CodecHostUnregister(&D->Host, sp->FileObject);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_PNA2DP_CODEC_GET_WORK:
        status = CodecHostQueueGetWork(&D->Host, Irp);
        if (status == STATUS_PENDING) return STATUS_PENDING;
        break;
    case IOCTL_PNA2DP_CODEC_PUT_RESULT: {
        if (!D->Host.Registered || D->Host.OwnerFile != sp->FileObject) { status = STATUS_ACCESS_DENIED; break; }
        if (inLen < sizeof(PNA2DP_CODEC_RESULT)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        const PNA2DP_CODEC_RESULT *r = (const PNA2DP_CODEC_RESULT *)buf;
        if (r->Size != inLen || r->PayloadBytes + sizeof(*r) > inLen) { status = STATUS_INVALID_PARAMETER; break; }
        status = PumpHostResult(&D->Pump, r, (const UCHAR *)(r + 1));
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    return CompleteIrp(Irp, status, NT_SUCCESS(status) ? info : 0);
}
