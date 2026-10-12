/*
 * bthlink.cpp - BthPort profile-driver DDI wrapper: BRBs, L2CAP channels, SDP, PnP events.
 *
 * Patterns follow the Microsoft "bthecho" sample (MIT) but the objects here are plain
 * structures embedded in the device context instead of WDF objects, because the FDO is
 * owned by PortCls and we only have a WDF miniport device.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

/* ------------------------------------------------------------------------- */
/* Generic BRB submission                                                      */
/* ------------------------------------------------------------------------- */

/* Each long-lived request owns one preallocated memory object that is re-pointed at the BRB
 * for every send. Creating a new object per send would leak one framework object per
 * packet, because children of a request live until the request is deleted. */
static NTSTATUS PnFormatRequestWithBrb(_In_ WDFIOTARGET Target, _In_ WDFREQUEST Request, _Inout_ WDFMEMORY *BrbMemory,
                                       _In_ PBRB Brb, _In_ size_t BrbSize)
{
    if (!BrbSize) return STATUS_INVALID_PARAMETER;
    NTSTATUS status;
    if (*BrbMemory == NULL) {
        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = Request;
        status = WdfMemoryCreatePreallocated(&attr, Brb, BrbSize, BrbMemory);
        if (!NT_SUCCESS(status)) { *BrbMemory = NULL; return status; }
    } else {
        status = WdfMemoryAssignBuffer(*BrbMemory, Brb, BrbSize);
        if (!NT_SUCCESS(status)) return status;
    }
    return WdfIoTargetFormatRequestForInternalIoctlOthers(Target, Request, IOCTL_INTERNAL_BTH_SUBMIT_BRB,
                                                         *BrbMemory, NULL, NULL, NULL, NULL, NULL);
}

static NTSTATUS PnSendBrbAsync(_In_ WDFIOTARGET Target, _In_ WDFREQUEST Request, _Inout_ WDFMEMORY *BrbMemory, _In_ PBRB Brb,
                               _In_ size_t BrbSize, _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion, _In_opt_ WDFCONTEXT Context)
{
    WDF_REQUEST_REUSE_PARAMS reuse;
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
    NTSTATUS status = WdfRequestReuse(Request, &reuse);
    if (!NT_SUCCESS(status)) return status;
    status = PnFormatRequestWithBrb(Target, Request, BrbMemory, Brb, BrbSize);
    if (!NT_SUCCESS(status)) return status;
    WdfRequestSetCompletionRoutine(Request, Completion, Context);
    if (!WdfRequestSend(Request, Target, NULL)) {
        status = WdfRequestGetStatus(Request);
        PN_ERR("WdfRequestSend(BRB %u) failed 0x%08x", Brb->BrbHeader.Type, status);
        return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS PnLinkSendBrbSync(_In_ PPN_BTH_LINK Link, _In_ PBRB Brb, _In_ ULONG BrbSize)
{
    PAGED_CODE();
    WDF_REQUEST_REUSE_PARAMS reuse;
    WDF_MEMORY_DESCRIPTOR desc;
    PnMutexAcquire(&Link->SyncRequestMutex);
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
    NTSTATUS status = WdfRequestReuse(Link->SyncRequest, &reuse);
    if (NT_SUCCESS(status)) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, Brb, BrbSize);
        status = WdfIoTargetSendInternalIoctlOthersSynchronously(Link->IoTarget, Link->SyncRequest,
                                                                 IOCTL_INTERNAL_BTH_SUBMIT_BRB, &desc, NULL, NULL, NULL, NULL);
    }
    PnMutexRelease(&Link->SyncRequestMutex);
    return status;
}

/* ------------------------------------------------------------------------- */
/* Query the profile driver DDI with a PnP IRP (we are not a WDF FDO)          */
/* ------------------------------------------------------------------------- */

NTSTATUS PnLinkEnsureActive(_Inout_ PPN_BTH_LINK Link)
{
    PAGED_CODE();
    struct _BRB_ACL_GET_MODE mode;
    Link->Ddi.BthInitializeBrb((PBRB)&mode, BRB_ACL_GET_MODE);
    mode.BtAddress = Link->RemoteAddress;
    NTSTATUS status = PnLinkSendBrbSync(Link, (PBRB)&mode, sizeof(mode));
    if (!NT_SUCCESS(status)) { PN_WARN("ACL mode query failed 0x%08x", status); return status; }
    if (mode.AclMode == ACL_MODE_ACTIVE) return STATUS_SUCCESS;

    struct _BRB_ACL_ENTER_ACTIVE_MODE active;
    Link->Ddi.BthInitializeBrb((PBRB)&active, BRB_ACL_ENTER_ACTIVE_MODE);
    active.BtAddress = Link->RemoteAddress;
    status = PnLinkSendBrbSync(Link, (PBRB)&active, sizeof(active));
    PN_INFO("ACL mode %u -> active: 0x%08x", (ULONG)mode.AclMode, status);
    return status;
}

static NTSTATUS PnQueryInterface(_In_ PDEVICE_OBJECT Target, _In_ const GUID *InterfaceType, _In_ USHORT Size,
                                 _In_ USHORT Version, _Out_ PINTERFACE Interface)
{
    PAGED_CODE();
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    PIRP irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, Target, NULL, 0, NULL, &event, &iosb);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(irp);
    sp->MinorFunction = IRP_MN_QUERY_INTERFACE;
    sp->Parameters.QueryInterface.InterfaceType = InterfaceType;
    sp->Parameters.QueryInterface.Size = Size;
    sp->Parameters.QueryInterface.Version = Version;
    sp->Parameters.QueryInterface.Interface = Interface;
    sp->Parameters.QueryInterface.InterfaceSpecificData = NULL;
    NTSTATUS status = IoCallDriver(Target, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}

/* ------------------------------------------------------------------------- */
/* Link lifecycle                                                               */
/* ------------------------------------------------------------------------- */

NTSTATUS PnLinkInitialize(_Inout_ PPN_BTH_LINK Link, _In_ WDFDEVICE WdfDevice, _In_ PDEVICE_OBJECT Fdo, _In_ PDEVICE_OBJECT Pdo)
{
    PAGED_CODE();
    RtlZeroMemory(Link, sizeof(*Link));
    Link->WdfDevice = WdfDevice;
    Link->Fdo = Fdo;
    Link->Pdo = Pdo;
    Link->IoTarget = WdfDeviceGetIoTarget(WdfDevice);
    ExInitializeFastMutex(&Link->SyncRequestMutex);
    if (!Link->IoTarget) {
        PN_ERR("miniport device has no default I/O target");
        return STATUS_INVALID_DEVICE_STATE;
    }

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = WdfDevice;
    NTSTATUS status = WdfRequestCreate(&attr, Link->IoTarget, &Link->SyncRequest);
    if (!NT_SUCCESS(status)) return status;

    /* Profile driver interface: ask the stack below the FDO. */
    status = PnQueryInterface(Pdo, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE, sizeof(Link->Ddi),
                              BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, (PINTERFACE)&Link->Ddi);
    if (!NT_SUCCESS(status)) {
        PN_ERR("GUID_BTHDDI_PROFILE_DRIVER_INTERFACE query failed 0x%08x", status);
        return status;
    }
    Link->DdiValid = TRUE;

    /* Local radio address */
    struct _BRB_GET_LOCAL_BD_ADDR *brb = (struct _BRB_GET_LOCAL_BD_ADDR *)
        Link->Ddi.BthAllocateBrb(BRB_HCI_GET_LOCAL_BD_ADDR, PNA2DP_TAG_BRB);
    if (!brb) return STATUS_INSUFFICIENT_RESOURCES;
    status = PnLinkSendBrbSync(Link, (PBRB)brb, sizeof(*brb));
    if (NT_SUCCESS(status)) Link->LocalAddress = brb->BtAddress;
    Link->Ddi.BthFreeBrb((PBRB)brb);
    if (!NT_SUCCESS(status)) {
        PN_WARN("BRB_HCI_GET_LOCAL_BD_ADDR failed 0x%08x", status);
    }

    /* Host features (ERTM etc.) - informational */
    {
        WDF_MEMORY_DESCRIPTOR out;
        BTH_HOST_FEATURE_MASK mask = {0};
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &mask, sizeof(mask));
        if (NT_SUCCESS(WdfIoTargetSendIoctlSynchronously(Link->IoTarget, NULL, IOCTL_BTH_GET_HOST_SUPPORTED_FEATURES,
                                                         NULL, &out, NULL, NULL))) {
            Link->HostFeatures = mask;
        }
    }

    status = PnLinkQueryRemoteInfo(Link);
    if (!NT_SUCCESS(status)) {
        PN_ERR("IOCTL_INTERNAL_BTHENUM_GET_DEVINFO failed 0x%08x", status);
        return status;
    }
    PN_INFO("link up: local %012llX remote %012llX '%s'", Link->LocalAddress, Link->RemoteAddress, Link->RemoteName);
    return STATUS_SUCCESS;
}

VOID PnLinkUninitialize(_Inout_ PPN_BTH_LINK Link)
{
    PAGED_CODE();
    PnLinkUnregisterRadioEvents(Link);
    PnLinkUnregisterServer(Link);
    if (Link->SyncRequest) {
        WdfObjectDelete(Link->SyncRequest);
        Link->SyncRequest = NULL;
    }
    if (Link->DdiValid && Link->Ddi.Interface.InterfaceDereference) {
        Link->Ddi.Interface.InterfaceDereference(Link->Ddi.Interface.Context);   /* taken by IRP_MN_QUERY_INTERFACE */
    }
    Link->DdiValid = FALSE;
}

NTSTATUS PnLinkQueryRemoteInfo(_Inout_ PPN_BTH_LINK Link)
{
    PAGED_CODE();
    BTH_DEVICE_INFO info;
    WDF_MEMORY_DESCRIPTOR out;
    RtlZeroMemory(&info, sizeof(info));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &info, sizeof(info));
    NTSTATUS status = WdfIoTargetSendInternalIoctlSynchronously(Link->IoTarget, NULL, IOCTL_INTERNAL_BTHENUM_GET_DEVINFO,
                                                                NULL, &out, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;
    Link->RemoteAddress = info.address;
    Link->RemoteCod = info.classOfDevice;
    RtlStringCbCopyA(Link->RemoteName, sizeof(Link->RemoteName), info.name);
    size_t nameBytes = 0;
    RtlStringCbLengthA(Link->RemoteName, sizeof(Link->RemoteName), &nameBytes);
    ULONG written = 0;
    NTSTATUS nameStatus = RtlUTF8ToUnicodeN(Link->RemoteNameW, sizeof(Link->RemoteNameW) - sizeof(WCHAR),
                                           &written, Link->RemoteName, (ULONG)nameBytes);
    if (NT_SUCCESS(nameStatus) && written) Link->RemoteNameW[written / sizeof(WCHAR)] = 0;
    else RtlStringCchCopyW(Link->RemoteNameW, PN_ARRAYSIZE(Link->RemoteNameW), L"Bluetooth Audio");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* SDP: read the remote AudioSink / AudioSource record                          */
/* ------------------------------------------------------------------------- */

/* SDP data element header (Bluetooth Core, Vol 3, Part B, 3.3). The response comes from
 * the remote device, so every length is checked against the bytes that remain. */
#define PN_SDP_TYPE_UINT      1
#define PN_SDP_TYPE_UUID      3
#define PN_SDP_TYPE_SEQUENCE  6

static BOOLEAN PnSdpElement(_In_reads_bytes_(Len) const UCHAR *P, _In_ ULONG Len, _Out_ PUCHAR Type,
                            _Out_ const UCHAR **Data, _Out_ PULONG DataLen, _Out_ PULONG Total)
{
    *Type = 0; *Data = NULL; *DataLen = 0; *Total = 0;
    if (Len < 1) return FALSE;
    UCHAR type = (UCHAR)(P[0] >> 3), si = (UCHAR)(P[0] & 7);
    ULONG hdr = 1, dl;
    if (type == 0) dl = 0;
    else if (si <= 4) dl = 1u << si;
    else if (si == 5) { if (Len < 2) return FALSE; dl = P[1]; hdr = 2; }
    else if (si == 6) { if (Len < 3) return FALSE; dl = ((ULONG)P[1] << 8) | P[2]; hdr = 3; }
    else { if (Len < 5) return FALSE; dl = ((ULONG)P[1] << 24) | ((ULONG)P[2] << 16) | ((ULONG)P[3] << 8) | P[4]; hdr = 5; }
    if (dl > Len - hdr) return FALSE;
    *Type = type; *Data = P + hdr; *DataLen = dl; *Total = hdr + dl;
    return TRUE;
}

static BOOLEAN PnSdpUint16(_In_ UCHAR Type, _In_reads_bytes_(Len) const UCHAR *D, _In_ ULONG Len, _Out_ PUSHORT Value)
{
    *Value = 0;
    if ((Type != PN_SDP_TYPE_UINT && Type != PN_SDP_TYPE_UUID) || Len != 2) return FALSE;
    *Value = (USHORT)(((USHORT)D[0] << 8) | D[1]);
    return TRUE;
}

/* Profile descriptor list: seq( seq( uuid16 0x110D, uint16 version ) ... ) */
static VOID PnSdpParseProfileList(_In_ PPN_BTH_LINK Link, _In_reads_bytes_(Len) const UCHAR *P, _In_ ULONG Len)
{
    while (Len) {
        UCHAR t; const UCHAR *d; ULONG dl, tot;
        if (!PnSdpElement(P, Len, &t, &d, &dl, &tot)) return;
        if (t == PN_SDP_TYPE_SEQUENCE) {
            UCHAR t1, t2; const UCHAR *d1, *d2; ULONG l1, l2, n1, n2;
            USHORT uuid, ver;
            if (PnSdpElement(d, dl, &t1, &d1, &l1, &n1) && PnSdpUint16(t1, d1, l1, &uuid) && uuid == A2DP_UUID_ADVANCED_AUDIO &&
                PnSdpElement(d + n1, dl - n1, &t2, &d2, &l2, &n2) && PnSdpUint16(t2, d2, l2, &ver)) {
                Link->RemoteAvdtpVersion = ver;
                return;
            }
        }
        P += tot; Len -= tot;
    }
}

static NTSTATUS PnSdpParseRecord(_In_ PPN_BTH_LINK Link, _In_reads_bytes_(Len) const UCHAR *P, _In_ ULONG Len)
{
    PAGED_CODE();
    UCHAR t; const UCHAR *d; ULONG dl, tot;
    /* AttributeLists: seq( seq(attribute list of record 1) ... ); accept a bare list too */
    if (!PnSdpElement(P, Len, &t, &d, &dl, &tot) || t != PN_SDP_TYPE_SEQUENCE) return STATUS_DEVICE_DATA_ERROR;
    {
        UCHAR t1; const UCHAR *d1; ULONG l1, n1;
        if (PnSdpElement(d, dl, &t1, &d1, &l1, &n1) && t1 == PN_SDP_TYPE_SEQUENCE) { d = d1; dl = l1; }
    }
    /* attribute list: uint16 id, value, uint16 id, value, ... */
    while (dl) {
        UCHAR ti, tv; const UCHAR *di, *dv; ULONG li, lv, ni, nv;
        USHORT id;
        if (!PnSdpElement(d, dl, &ti, &di, &li, &ni) || !PnSdpUint16(ti, di, li, &id)) break;
        if (!PnSdpElement(d + ni, dl - ni, &tv, &dv, &lv, &nv)) break;
        if (id == SDP_ATTRIB_PROFILE_DESCRIPTOR_LIST && tv == PN_SDP_TYPE_SEQUENCE) PnSdpParseProfileList(Link, dv, lv);
        else if (id == 0x0311) PnSdpUint16(tv, dv, lv, &Link->RemoteSupportedFeatures);   /* SupportedFeatures */
        d += ni + nv; dl -= ni + nv;
    }
    return STATUS_SUCCESS;
}

NTSTATUS PnLinkQueryRemoteSdp(_Inout_ PPN_BTH_LINK Link, _In_ USHORT ServiceUuid16)
{
    PAGED_CODE();
    NTSTATUS status;
    WDF_MEMORY_DESCRIPTOR in, out;
    BTH_SDP_CONNECT connect = {0};
    BTH_SDP_DISCONNECT disconnect = {0};
    BTH_SDP_SERVICE_ATTRIBUTE_SEARCH_REQUEST req = {0};
    BTH_SDP_STREAM_RESPONSE probe = {0};
    PBTH_SDP_STREAM_RESPONSE rec = NULL;
    WDFREQUEST request = NULL;
    WDF_OBJECT_ATTRIBUTES attr;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    status = WdfRequestCreate(&attr, Link->IoTarget, &request);
    if (!NT_SUCCESS(status)) return status;

    connect.bthAddress = Link->RemoteAddress;
    connect.requestTimeout = SDP_REQUEST_TO_DEFAULT;
    connect.fSdpConnect = 0;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &connect, sizeof(connect));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &connect, sizeof(connect));
    status = WdfIoTargetSendIoctlSynchronously(Link->IoTarget, request, IOCTL_BTH_SDP_CONNECT, &in, &out, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        PN_WARN("SDP connect to %012llX failed 0x%08x", Link->RemoteAddress, status);
        WdfObjectDelete(request);
        return status;
    }

    req.hConnection = connect.hConnection;
    req.uuids[0].u.uuid16 = ServiceUuid16;
    req.uuids[0].uuidType = SDP_ST_UUID16;
    req.range[0].minAttribute = 0;
    req.range[0].maxAttribute = 0xFFFF;

    WDF_REQUEST_REUSE_PARAMS reuse;
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
    WdfRequestReuse(request, &reuse);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &req, sizeof(req));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &probe, sizeof(probe));
    status = WdfIoTargetSendIoctlSynchronously(Link->IoTarget, request, IOCTL_BTH_SDP_SERVICE_ATTRIBUTE_SEARCH, &in, &out, NULL, NULL);
    if (NT_SUCCESS(status) || status == STATUS_BUFFER_TOO_SMALL || status == STATUS_BUFFER_OVERFLOW) {
        ULONG size = 0;
        if (NT_SUCCESS(RtlULongAdd(probe.requiredSize, sizeof(BTH_SDP_STREAM_RESPONSE), &size)) && size < 65536) {
            rec = (PBTH_SDP_STREAM_RESPONSE)PnAllocNp(size, PNA2DP_TAG_SDP);
            if (rec) {
                WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
                WdfRequestReuse(request, &reuse);
                WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &req, sizeof(req));
                WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, rec, size);
                status = WdfIoTargetSendIoctlSynchronously(Link->IoTarget, request, IOCTL_BTH_SDP_SERVICE_ATTRIBUTE_SEARCH,
                                                           &in, &out, NULL, NULL);
                if (NT_SUCCESS(status)) {
                    ULONG cap = size - (ULONG)FIELD_OFFSET(BTH_SDP_STREAM_RESPONSE, response);
                    status = PnSdpParseRecord(Link, rec->response, PN_MIN(rec->responseSize, cap));
                }
                PnFree(rec, PNA2DP_TAG_SDP);
            } else {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
        } else {
            status = STATUS_DEVICE_DATA_ERROR;
        }
    }

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
    WdfRequestReuse(request, &reuse);
    disconnect.hConnection = connect.hConnection;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &disconnect, sizeof(disconnect));
    WdfIoTargetSendIoctlSynchronously(Link->IoTarget, request, IOCTL_BTH_SDP_DISCONNECT, &in, NULL, NULL, NULL);
    WdfObjectDelete(request);

    if (NT_SUCCESS(status)) {
        PN_INFO("SDP %04X: AVDTP version %04X features %04X", ServiceUuid16, Link->RemoteAvdtpVersion,
                Link->RemoteSupportedFeatures);
    }
    return status;
}

/* ------------------------------------------------------------------------- */
/* Incoming connections: PSM + L2CAP server for our remote address             */
/* ------------------------------------------------------------------------- */

static VOID PnLinkServerIndication(_In_ PVOID Context, _In_ INDICATION_CODE Indication, _In_ PINDICATION_PARAMETERS Params)
{
    PPN_BTH_LINK link = (PPN_BTH_LINK)Context;
    PFN_PN_LINK_INCOMING onIncoming = link->OnIncoming;
    if (!onIncoming) return;   /* unregistering: BthPort times the connect out */
    switch (Indication) {
    case IndicationRemoteConnect:
        if (Params->BtAddress != link->RemoteAddress) {
            /* Not our device: let BthPort time it out; a reject needs a channel object. */
            PN_WARN("connect indication from foreign address %012llX ignored", Params->BtAddress);
            PnLinkRejectIncoming(link, Params);
            return;
        }
        onIncoming(link->CallbackContext, Params);
        break;
    default:
        break;
    }
}

NTSTATUS PnLinkRegisterServer(_Inout_ PPN_BTH_LINK Link, _In_ PFN_PN_LINK_INCOMING OnIncoming, _In_ PVOID Context)
{
    PAGED_CODE();
    NTSTATUS status;
    Link->OnIncoming = OnIncoming;
    Link->CallbackContext = Context;

    /* The AVDTP PSM may already be registered by the inbox driver for another device;
     * registration is best effort. */
    Link->Ddi.BthInitializeBrb((PBRB)&Link->PsmBrb, BRB_REGISTER_PSM);
    Link->PsmBrb.Psm = PSM_AVDTP;
    status = PnLinkSendBrbSync(Link, (PBRB)&Link->PsmBrb, sizeof(Link->PsmBrb));
    if (NT_SUCCESS(status)) {
        Link->Psm = Link->PsmBrb.Psm;
        PN_INFO("PSM 0x%04X registered", Link->Psm);
    } else {
        PN_WARN("BRB_REGISTER_PSM(0x19) failed 0x%08x (probably already registered); continuing", status);
        Link->Psm = 0;
    }

    Link->Ddi.BthInitializeBrb((PBRB)&Link->RegBrb, BRB_L2CA_REGISTER_SERVER);
    Link->RegBrb.BtAddress = Link->RemoteAddress;      /* only connections from our device */
    Link->RegBrb.PSM = PSM_AVDTP;
    Link->RegBrb.IndicationCallback = PnLinkServerIndication;
    Link->RegBrb.IndicationCallbackContext = Link;
    Link->RegBrb.IndicationFlags = 0;
    Link->RegBrb.ReferenceObject = Link->Fdo;
    status = PnLinkSendBrbSync(Link, (PBRB)&Link->RegBrb, sizeof(Link->RegBrb));
    if (!NT_SUCCESS(status)) {
        PN_ERR("BRB_L2CA_REGISTER_SERVER failed 0x%08x", status);
        Link->ServerHandle = NULL;
        return status;
    }
    Link->ServerHandle = Link->RegBrb.ServerHandle;
    PN_INFO("L2CAP server registered for %012llX", Link->RemoteAddress);
    return STATUS_SUCCESS;
}

BOOLEAN PnLinkUnregisterServer(_Inout_ PPN_BTH_LINK Link)
{
    PAGED_CODE();
    BOOLEAN stuck = FALSE;
    if (!Link->DdiValid) return FALSE;
    Link->OnIncoming = NULL;   /* indications that still arrive become no-ops */
    if (Link->ServerHandle) {
        struct _BRB_L2CA_UNREGISTER_SERVER brb = {};
        Link->Ddi.BthInitializeBrb((PBRB)&brb, BRB_L2CA_UNREGISTER_SERVER);
        brb.BtAddress = Link->RemoteAddress;
        brb.Psm = Link->RegBrb.PSM;
        brb.ServerHandle = Link->ServerHandle;
        NTSTATUS status = PnLinkSendBrbSync(Link, (PBRB)&brb, sizeof(brb));
        if (!NT_SUCCESS(status)) {
            PN_ERR("BRB_L2CA_UNREGISTER_SERVER failed 0x%08x; the server callback context stays allocated", status);
            stuck = TRUE;
        }
        Link->ServerHandle = NULL;
    }
    if (Link->Psm) {
        Link->Ddi.BthInitializeBrb((PBRB)&Link->PsmBrb, BRB_UNREGISTER_PSM);
        Link->PsmBrb.Psm = Link->Psm;
        PnLinkSendBrbSync(Link, (PBRB)&Link->PsmBrb, sizeof(Link->PsmBrb));
        Link->Psm = 0;
    }
    return stuck;
}

/* Reject with a one-shot BRB that frees itself on completion. */
typedef struct _PN_REJECT_CTX {
    PFNBTH_FREE_BRB FreeBrb;
    PBRB            Brb;
} PN_REJECT_CTX, *PPN_REJECT_CTX;
WDF_DECLARE_CONTEXT_TYPE(PN_REJECT_CTX);

static VOID PnRejectCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
                               _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);
    UNREFERENCED_PARAMETER(Context);
    PPN_REJECT_CTX ctx = WdfObjectGet_PN_REJECT_CTX(Request);
    if (ctx && ctx->Brb && ctx->FreeBrb) ctx->FreeBrb(ctx->Brb);
    WdfObjectDelete(Request);
}

NTSTATUS PnLinkRejectIncoming(_In_ PPN_BTH_LINK Link, _In_ PINDICATION_PARAMETERS Params)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDFREQUEST request = NULL;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, PN_REJECT_CTX);
    attr.ParentObject = Link->WdfDevice;
    NTSTATUS status = WdfRequestCreate(&attr, Link->IoTarget, &request);
    if (!NT_SUCCESS(status)) return status;
    struct _BRB_L2CA_OPEN_ENHANCED_CHANNEL *brb = (struct _BRB_L2CA_OPEN_ENHANCED_CHANNEL *)
        Link->Ddi.BthAllocateBrb(BRB_L2CA_OPEN_ENHANCED_CHANNEL_RESPONSE, PNA2DP_TAG_BRB);
    if (!brb) { WdfObjectDelete(request); return STATUS_INSUFFICIENT_RESOURCES; }
    PPN_REJECT_CTX ctx = WdfObjectGet_PN_REJECT_CTX(request);
    ctx->FreeBrb = Link->Ddi.BthFreeBrb;
    ctx->Brb = (PBRB)brb;
    brb->BtAddress = Params->BtAddress;
    brb->Psm = Params->Parameters.Connect.Request.PSM;
    brb->ChannelHandle = Params->ConnectionHandle;
    brb->Response = CONNECT_RSP_RESULT_NO_RESOURCES;
    brb->ChannelFlags = CF_ROLE_EITHER;
    brb->ReferenceObject = Link->Fdo;
    WDFMEMORY mem = NULL;   /* child of the one-shot request */
    status = PnSendBrbAsync(Link->IoTarget, request, &mem, (PBRB)brb, sizeof(*brb), PnRejectCompletion, Link);
    if (!NT_SUCCESS(status)) {
        Link->Ddi.BthFreeBrb((PBRB)brb);
        WdfObjectDelete(request);
    }
    return status;
}

/* ------------------------------------------------------------------------- */
/* Radio (BthPort) PnP custom notifications: HCI SCO/ACL events                */
/* ------------------------------------------------------------------------- */

static NTSTATUS PnLinkRadioNotification(_In_ PVOID NotificationStructure, _Inout_opt_ PVOID Context)
{
    PPN_BTH_LINK link = (PPN_BTH_LINK)Context;
    PTARGET_DEVICE_CUSTOM_NOTIFICATION n = (PTARGET_DEVICE_CUSTOM_NOTIFICATION)NotificationStructure;
    if (!link || !n) return STATUS_SUCCESS;

    if (IsEqualGUID(n->Event, GUID_BLUETOOTH_HCI_EVENT)) {
        if (n->Size >= FIELD_OFFSET(TARGET_DEVICE_CUSTOM_NOTIFICATION, CustomDataBuffer) + sizeof(BTH_HCI_EVENT_INFO)) {
            PBTH_HCI_EVENT_INFO e = (PBTH_HCI_EVENT_INFO)n->CustomDataBuffer;
            if (e->bthAddress == link->RemoteAddress) {
                if (e->connectionType == HCI_CONNECTION_TYPE_SCO) {
                    BOOLEAN active = e->connected != 0;
                    if (link->ScoActive != active) {
                        link->ScoActive = active;
                        PN_INFO("SCO %s for %012llX", active ? "up" : "down", e->bthAddress);
                        if (link->OnSco) link->OnSco(link->CallbackContext, active);
                    }
                } else if (e->connectionType == HCI_CONNECTION_TYPE_ACL) {
                    BOOLEAN connected = e->connected != 0;
                    if (link->AclConnected != connected) {
                        link->AclConnected = connected;
                        PN_INFO("ACL %s for %012llX", connected ? "up" : "down", e->bthAddress);
                        if (link->OnAcl) link->OnAcl(link->CallbackContext, connected);
                    }
                }
            }
        }
    } else if (IsEqualGUID(n->Event, GUID_TARGET_DEVICE_REMOVE_COMPLETE) || IsEqualGUID(n->Event, GUID_TARGET_DEVICE_QUERY_REMOVE)) {
        /* Radio going away: drop our file object reference so the removal can proceed. The
         * registration itself is removed by DeviceStop; IoUnregisterPlugPlayNotificationEx
         * waits for running callbacks and must not be called from this one. */
        PN_WARN("radio removal notification");
        PFILE_OBJECT file = (PFILE_OBJECT)InterlockedExchangePointer((PVOID *)&link->RadioFileObject, NULL);
        if (file) ObDereferenceObject(file);
        link->RadioDeviceObject = NULL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS PnQueryLocalRadio(PPN_BTH_LINK Link, PUNICODE_STRING Name, BTH_LOCAL_RADIO_INFO *Info)
{
    WDFIOTARGET target = NULL;
    NTSTATUS status = WdfIoTargetCreate(Link->WdfDevice, WDF_NO_OBJECT_ATTRIBUTES, &target);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_TARGET_OPEN_PARAMS open;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, Name, GENERIC_READ);
    open.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    status = WdfIoTargetOpen(target, &open);
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(Info, sizeof(*Info));
        WDF_MEMORY_DESCRIPTOR output;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, Info, sizeof(*Info));
        WDF_REQUEST_SEND_OPTIONS options;
        WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(2));
        ULONG_PTR returned = 0;
        status = WdfIoTargetSendIoctlSynchronously(target, NULL, IOCTL_BTH_GET_LOCAL_INFO,
            NULL, &output, &options, &returned);
        if (NT_SUCCESS(status) && returned < sizeof(*Info)) status = STATUS_INFO_LENGTH_MISMATCH;
        WdfIoTargetClose(target);
    }
    WdfObjectDelete(target);
    return status;
}

NTSTATUS PnLinkRegisterRadioEvents(_Inout_ PPN_BTH_LINK Link, _In_ PFN_PN_LINK_SCO OnSco, _In_ PFN_PN_LINK_ACL OnAcl, _In_ PVOID Context)
{
    PAGED_CODE();
    Link->RadioInfoValid = FALSE;
    Link->RadioQueryStatus = STATUS_DEVICE_NOT_CONNECTED;
    PWSTR list = NULL;
    NTSTATUS status = IoGetDeviceInterfaces(&GUID_BTHPORT_DEVICE_INTERFACE, NULL, 0, &list);
    if (!NT_SUCCESS(status) || !list) {
        PN_WARN("no BthPort device interface (0x%08x)", status);
        return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_CONNECTED : status;
    }
    if (list[0] == L'\0') {
        ExFreePool(list);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    PFILE_OBJECT file = NULL;
    PDEVICE_OBJECT dev = NULL;
    status = STATUS_DEVICE_NOT_CONNECTED;
    for (PWSTR item = list; *item; item += wcslen(item) + 1) {
        UNICODE_STRING name;
        RtlInitUnicodeString(&name, item);
        BTH_LOCAL_RADIO_INFO radio;
        NTSTATUS query = PnQueryLocalRadio(Link, &name, &radio);
        if (!NT_SUCCESS(query)) { Link->RadioQueryStatus = query; continue; }
        if (!Link->LocalAddress || radio.localInfo.address != Link->LocalAddress) continue;
        Link->RadioInfo = radio;
        Link->RadioInfoValid = TRUE;
        Link->RadioQueryStatus = STATUS_SUCCESS;
        status = IoGetDeviceObjectPointer(&name, FILE_READ_DATA, &file, &dev);
        PN_INFO("local radio %012llX: HCI %u LMP %u features %016llX", Link->LocalAddress,
            radio.hciVersion, radio.radioInfo.lmpVersion, radio.radioInfo.lmpSupportedFeatures);
        break;
    }
    ExFreePool(list);
    if (!NT_SUCCESS(status)) {
        PN_WARN("IoGetDeviceObjectPointer(radio) failed 0x%08x", status);
        return status;
    }
    Link->OnSco = OnSco;
    Link->OnAcl = OnAcl;
    Link->CallbackContext = Context;
    Link->RadioFileObject = file;
    Link->RadioDeviceObject = dev;
    status = IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange, 0, file, Link->Fdo->DriverObject,
                                            PnLinkRadioNotification, Link, &Link->PnpNotificationEntry);
    if (!NT_SUCCESS(status)) {
        PN_WARN("IoRegisterPlugPlayNotification failed 0x%08x", status);
        ObDereferenceObject(file);
        Link->RadioFileObject = NULL;
        Link->RadioDeviceObject = NULL;
        return status;
    }
    return STATUS_SUCCESS;
}

VOID PnLinkUnregisterRadioEvents(_Inout_ PPN_BTH_LINK Link)
{
    PAGED_CODE();
    PVOID entry = InterlockedExchangePointer(&Link->PnpNotificationEntry, NULL);
    if (entry) IoUnregisterPlugPlayNotificationEx(entry);
    PFILE_OBJECT file = (PFILE_OBJECT)InterlockedExchangePointer((PVOID *)&Link->RadioFileObject, NULL);
    if (file) ObDereferenceObject(file);
    Link->RadioDeviceObject = NULL;
}

/* ------------------------------------------------------------------------- */
/* L2CAP channel                                                               */
/* ------------------------------------------------------------------------- */

static VOID PnReaderSubmit(_In_ PPN_L2CAP_READER R);

static VOID PnReaderResubmitDpc(_In_ PKDPC Dpc, _In_opt_ PVOID Ctx, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(Ctx); UNREFERENCED_PARAMETER(Arg2);
    if (Arg1) PnReaderSubmit((PPN_L2CAP_READER)Arg1);
}

static VOID PnReaderCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
                               _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Request); UNREFERENCED_PARAMETER(Target);
    PPN_L2CAP_READER r = (PPN_L2CAP_READER)Context;
    PPN_L2CAP_CHANNEL ch = r->Channel;
    NTSTATUS status = Params->IoStatus.Status;
    InterlockedExchange(&r->Outstanding, 0);
    if (NT_SUCCESS(status)) {
        if (!r->Stopping && r->Generation == ch->Generation && ch->OnReceive && r->Brb.BufferSize <= r->BufferSize && r->Brb.BufferSize) {
            ch->OnReceive(ch->CallbackContext, ch, r->Buffer, r->Brb.BufferSize);
        }
        KeInsertQueueDpc(&r->ResubmitDpc, r, NULL);
    } else {
        if (status != STATUS_CANCELLED) {
            PN_WARN("L2CAP read failed 0x%08x on %s channel", status, ch->IsMedia ? "media" : "signalling");
            if (ch->OnClosed) ch->OnClosed(ch->CallbackContext, ch, status, r->Generation);
        }
        KeSetEvent(&r->StoppedEvent, 0, FALSE);
    }
}

static VOID PnReaderSubmitLocked(_In_ PPN_L2CAP_READER R)
{
    PPN_L2CAP_CHANNEL ch = R->Channel;
    PPN_BTH_LINK link = ch->Link;
    WDF_REQUEST_REUSE_PARAMS reuse;

    /* the chain ends when stopped or when its connection is gone or was replaced */
    if (R->Stopping || ch->State != PnChannelConnected || R->Generation != ch->Generation) {
        KeSetEvent(&R->StoppedEvent, 0, FALSE);
        return;
    }
    link->Ddi.BthReuseBrb((PBRB)&R->Brb, BRB_L2CA_ACL_TRANSFER);
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_UNSUCCESSFUL);
    NTSTATUS status = WdfRequestReuse(R->Request, &reuse);
    if (!NT_SUCCESS(status)) {
        PN_WARN("reader request reuse failed 0x%08x", status);   /* never resend a request that is not ours */
        KeSetEvent(&R->StoppedEvent, 0, FALSE);
        return;
    }
    R->Brb.BtAddress = ch->RemoteAddress;
    R->Brb.ChannelHandle = ch->Handle;
    R->Brb.TransferFlags = ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK;
    R->Brb.Buffer = R->Buffer;
    R->Brb.BufferMDL = NULL;
    R->Brb.BufferSize = R->BufferSize;
    status = PnFormatRequestWithBrb(link->IoTarget, R->Request, &R->BrbMemory, (PBRB)&R->Brb, sizeof(R->Brb));
    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(R->Request, PnReaderCompletion, R);
        KeClearEvent(&R->StoppedEvent);
        InterlockedExchange(&R->Outstanding, 1);
        /* PnChannelStopReaders sets Stopping and then reads Outstanding: re-checking after
         * publishing Outstanding means either this sees the stop or the stop cancels us */
        if (R->Stopping || ch->State != PnChannelConnected || R->Generation != ch->Generation) {
            InterlockedExchange(&R->Outstanding, 0);
            KeSetEvent(&R->StoppedEvent, 0, FALSE);
            return;
        }
        if (!WdfRequestSend(R->Request, link->IoTarget, NULL)) {
            InterlockedExchange(&R->Outstanding, 0);
            status = WdfRequestGetStatus(R->Request);
        } else {
            return;
        }
    }
    PN_WARN("reader submit failed 0x%08x", status);
    if (ch->OnClosed) ch->OnClosed(ch->CallbackContext, ch, status, R->Generation);
    KeSetEvent(&R->StoppedEvent, 0, FALSE);
}

static VOID PnReaderSubmit(_In_ PPN_L2CAP_READER R)
{
    KIRQL irql;
    KeAcquireSpinLock(&R->SendLock, &irql);
    PnReaderSubmitLocked(R);
    KeReleaseSpinLock(&R->SendLock, irql);
}

static VOID PnWriteCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
                              _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Request); UNREFERENCED_PARAMETER(Target);
    PPN_L2CAP_WRITE w = (PPN_L2CAP_WRITE)Context;
    PPN_L2CAP_CHANNEL ch = w->Channel;
    NTSTATUS status = Params->IoStatus.Status;
    ULONG bytes = w->Length;
    LONGLONG submitted = w->SubmitTimeHns;
    KIRQL irql;
    if (ch->OnWriteDone) ch->OnWriteDone(ch->CallbackContext, ch, status, bytes, submitted);
    /* PnChannelUninitialize waits for WritesInFlight == 0 before deleting the requests */
    KeAcquireSpinLock(&ch->Lock, &irql);
    RemoveEntryList(&w->Link);
    InsertTailList(&ch->FreeWrites, &w->Link);
    InterlockedExchange(&w->Outstanding, 0);
    InterlockedDecrement(&ch->WritesInFlight);
    KeReleaseSpinLock(&ch->Lock, irql);
}

NTSTATUS PnChannelInitialize(_Out_ PPN_L2CAP_CHANNEL Ch, _In_ PPN_BTH_LINK Link, _In_ BOOLEAN IsMedia, _In_ ULONG ReadBufferSize,
                             _In_ ULONG WriteCapacity, _In_ PFN_PN_L2CAP_RECEIVE OnReceive, _In_ PFN_PN_L2CAP_EVENT OnClosed,
                             _In_opt_ PFN_PN_L2CAP_WRITE_DONE OnWriteDone, _In_ PVOID Context)
{
    PAGED_CODE();
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attr;
    RtlZeroMemory(Ch, sizeof(*Ch));
    Ch->Link = Link;
    Ch->IsMedia = IsMedia;
    Ch->State = PnChannelFree;
    Ch->OnReceive = OnReceive;
    Ch->OnClosed = OnClosed;
    Ch->OnWriteDone = OnWriteDone;
    Ch->CallbackContext = Context;
    Ch->WriteCapacity = WriteCapacity;
    KeInitializeSpinLock(&Ch->Lock);
    ExInitializeFastMutex(&Ch->StartMutex);
    KeInitializeEvent(&Ch->DisconnectedEvent, NotificationEvent, TRUE);
    KeInitializeEvent(&Ch->ConnectedEvent, NotificationEvent, FALSE);
    InitializeListHead(&Ch->FreeWrites);
    InitializeListHead(&Ch->BusyWrites);

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Link->WdfDevice;
    status = WdfRequestCreate(&attr, Link->IoTarget, &Ch->OpenCloseRequest);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Link->WdfDevice;
    status = WdfRequestCreate(&attr, Link->IoTarget, &Ch->CloseRequest);
    if (!NT_SUCCESS(status)) return status;

    for (ULONG i = 0; i < PN_L2CAP_READERS; i++) {
        PPN_L2CAP_READER r = &Ch->Readers[i];
        r->Channel = Ch;
        r->BufferSize = ReadBufferSize;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = Link->WdfDevice;
        status = WdfRequestCreate(&attr, Link->IoTarget, &r->Request);
        if (!NT_SUCCESS(status)) return status;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = r->Request;
        status = WdfMemoryCreate(&attr, NonPagedPoolNx, PNA2DP_TAG_L2CAP, ReadBufferSize, &r->Memory, (PVOID *)&r->Buffer);
        if (!NT_SUCCESS(status)) return status;
        KeInitializeDpc(&r->ResubmitDpc, PnReaderResubmitDpc, NULL);
        KeInitializeSpinLock(&r->SendLock);
        KeInitializeEvent(&r->StoppedEvent, NotificationEvent, TRUE);
        Ch->ReadersInitialized++;
    }
    for (ULONG i = 0; i < PN_L2CAP_MAX_WRITES; i++) {
        PPN_L2CAP_WRITE w = &Ch->Writes[i];
        w->Channel = Ch;
        w->Capacity = WriteCapacity;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = Link->WdfDevice;
        status = WdfRequestCreate(&attr, Link->IoTarget, &w->Request);
        if (!NT_SUCCESS(status)) return status;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = w->Request;
        status = WdfMemoryCreate(&attr, NonPagedPoolNx, PNA2DP_TAG_L2CAP, WriteCapacity, &w->Memory, (PVOID *)&w->Buffer);
        if (!NT_SUCCESS(status)) return status;
        InsertTailList(&Ch->FreeWrites, &w->Link);
    }
    return STATUS_SUCCESS;
}

VOID PnChannelUninitialize(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    PAGED_CODE();
    if (!Ch->Link) return;
    PnChannelDisconnectSync(Ch);
    PnChannelStopReaders(Ch);   /* cancel any read still pending from an earlier connection */
    for (ULONG i = 0; i < Ch->ReadersInitialized; i++) {
        KeWaitForSingleObject(&Ch->Readers[i].StoppedEvent, Executive, KernelMode, FALSE, NULL);
        if (Ch->Readers[i].Request) { WdfObjectDelete(Ch->Readers[i].Request); Ch->Readers[i].Request = NULL; }
    }
    /* Outstanding writes complete once the channel is closed; cancel stragglers and wait,
     * because deleting a request that is still owned by the target is a framework violation. */
    LARGE_INTEGER wait;
    wait.QuadPart = -PN_HNS_PER_MS * 20;
    for (ULONG round = 0; Ch->WritesInFlight > 0; round++) {
        if (round % 50 == 0) {
            for (ULONG i = 0; i < PN_L2CAP_MAX_WRITES; i++) {
                if (Ch->Writes[i].Outstanding && Ch->Writes[i].Request) WdfRequestCancelSentRequest(Ch->Writes[i].Request);
            }
            if (round) PN_WARN("%s channel: %d writes still in flight", Ch->IsMedia ? "media" : "signalling", Ch->WritesInFlight);
        }
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
    }
    for (ULONG i = 0; i < PN_L2CAP_MAX_WRITES; i++) {
        if (Ch->Writes[i].Request) { WdfObjectDelete(Ch->Writes[i].Request); Ch->Writes[i].Request = NULL; }
    }
    if (Ch->OpenCloseRequest) { WdfObjectDelete(Ch->OpenCloseRequest); Ch->OpenCloseRequest = NULL; }
    if (Ch->CloseRequest) { WdfObjectDelete(Ch->CloseRequest); Ch->CloseRequest = NULL; }
    Ch->Link = NULL;
}

BOOLEAN PnChannelIsConnected(_In_ PPN_L2CAP_CHANNEL Ch)
{
    return Ch->State == PnChannelConnected;
}

LONG PnChannelWritesInFlight(_In_ PPN_L2CAP_CHANNEL Ch)
{
    return Ch->WritesInFlight;
}

static VOID PnChannelIndication(_In_ PVOID Context, _In_ INDICATION_CODE Indication, _In_ PINDICATION_PARAMETERS_ENHANCED Params)
{
    PPN_L2CAP_CHANNEL ch = (PPN_L2CAP_CHANNEL)Context;
    switch (Indication) {
    case IndicationRemoteDisconnect: {
        /* The channel object is reused across connections: act only if the indication is
         * for the connection that is current, and only on that generation. If we are
         * already closing, the closer owns the state change and no down event is raised. */
        KIRQL irql;
        KeAcquireSpinLock(&ch->Lock, &irql);
        BOOLEAN mine = Params && Params->ConnectionHandle &&
                       ((ch->State == PnChannelConnected && Params->ConnectionHandle == ch->Handle) ||
                        (ch->State == PnChannelConnecting && Params->ConnectionHandle == ch->OpenBrb.ChannelHandle));
        ULONG gen = ch->Generation;
        KeReleaseSpinLock(&ch->Lock, irql);
        PN_INFO("remote closed %s channel%s", ch->IsMedia ? "media" : "signalling", mine ? "" : " (not the current connection)");
        if (mine && PnChannelDisconnectGen(ch, gen) && ch->OnClosed) ch->OnClosed(ch->CallbackContext, ch, STATUS_CONNECTION_DISCONNECTED, gen);
        break;
    }
    default:
        break;
    }
}

static VOID PnFillOpenConfig(_Inout_ struct _BRB_L2CA_OPEN_ENHANCED_CHANNEL *Brb, _In_ USHORT PreferredMtu)
{
    Brb->ChannelFlags = CF_ROLE_EITHER;
    Brb->ConfigOut.Flags = CFG_MTU;
    Brb->ConfigOut.Mtu.Max = PreferredMtu;
    Brb->ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
    Brb->ConfigOut.Mtu.Preferred = PreferredMtu;
    Brb->ConfigIn.Flags = CFG_MTU;
    Brb->ConfigIn.Mtu.Max = PreferredMtu;
    Brb->ConfigIn.Mtu.Min = L2CAP_MIN_MTU;
    Brb->ConfigIn.Mtu.Preferred = PreferredMtu;
    Brb->CallbackFlags = CALLBACK_DISCONNECT;
    Brb->Callback = PnChannelIndication;
    Brb->IncomingQueueDepth = 16;
}

static VOID PnOpenCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
                             _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Request); UNREFERENCED_PARAMETER(Target);
    PPN_L2CAP_CHANNEL ch = (PPN_L2CAP_CHANNEL)Context;
    NTSTATUS status = Params->IoStatus.Status;
    KIRQL irql;
    BOOLEAN disconnectNow = FALSE;
    KeAcquireSpinLock(&ch->Lock, &irql);
    if (NT_SUCCESS(status)) {
        ch->Handle = ch->OpenBrb.ChannelHandle;
        ch->OutMtu = ch->OpenBrb.OutResults.Params.Mtu;
        ch->InMtu = ch->OpenBrb.InResults.Params.Mtu;
        ch->RemoteAddress = ch->OpenBrb.BtAddress;
        if (ch->State == PnChannelDisconnecting) {
            disconnectNow = TRUE;   /* a disconnect raced the open */
        }
        ch->State = PnChannelConnected;
    } else {
        ch->State = PnChannelDisconnected;
    }
    ch->OpenStatus = status;
    /* signal under the lock, so a new connect/accept that sees the new state and clears the
     * events cannot be overtaken by this completion's signals */
    KeSetEvent(&ch->ConnectedEvent, 0, FALSE);
    if (!NT_SUCCESS(status)) KeSetEvent(&ch->DisconnectedEvent, 0, FALSE);
    KeReleaseSpinLock(&ch->Lock, irql);
    PN_INFO("%s channel open: 0x%08x mtu out %u in %u", ch->IsMedia ? "media" : "signalling", status, ch->OutMtu, ch->InMtu);
    if (disconnectNow) {
        PnChannelDisconnect(ch);
    }
}

NTSTATUS PnChannelConnect(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ USHORT Psm, _In_ USHORT PreferredMtu, _In_ ULONG TimeoutMs)
{
    PAGED_CODE();
    PPN_BTH_LINK link = Ch->Link;
    KIRQL irql;
    KeAcquireSpinLock(&Ch->Lock, &irql);
    if (Ch->State != PnChannelFree && Ch->State != PnChannelDisconnected) {
        KeReleaseSpinLock(&Ch->Lock, irql);
        return STATUS_INVALID_DEVICE_STATE;
    }
    Ch->State = PnChannelConnecting;
    Ch->RemoteInitiated = FALSE;
    Ch->OpenStatus = STATUS_PENDING;
    Ch->Generation++;
    Ch->ReadersStarted = FALSE;
    KeClearEvent(&Ch->ConnectedEvent);
    KeClearEvent(&Ch->DisconnectedEvent);
    KeReleaseSpinLock(&Ch->Lock, irql);

    struct _BRB_L2CA_OPEN_ENHANCED_CHANNEL *brb = &Ch->OpenBrb;
    link->Ddi.BthInitializeBrb((PBRB)brb, BRB_L2CA_OPEN_ENHANCED_CHANNEL);
    brb->Hdr.ClientContext[0] = Ch;
    brb->BtAddress = link->RemoteAddress;
    brb->Psm = Psm;
    PnFillOpenConfig(brb, PreferredMtu);
    brb->CallbackContext = Ch;
    brb->ReferenceObject = link->Fdo;

    NTSTATUS status = PnSendBrbAsync(link->IoTarget, Ch->OpenCloseRequest, &Ch->OpenCloseBrbMemory, (PBRB)brb, sizeof(*brb), PnOpenCompletion, Ch);
    if (!NT_SUCCESS(status)) {
        KeAcquireSpinLock(&Ch->Lock, &irql);
        Ch->State = PnChannelDisconnected;
        Ch->OpenStatus = status;
        KeSetEvent(&Ch->DisconnectedEvent, 0, FALSE);
        KeSetEvent(&Ch->ConnectedEvent, 0, FALSE);
        KeReleaseSpinLock(&Ch->Lock, irql);
        return status;
    }
    LARGE_INTEGER timeout;
    timeout.QuadPart = -PN_HNS_PER_MS * (LONGLONG)TimeoutMs;
    status = KeWaitForSingleObject(&Ch->ConnectedEvent, Executive, KernelMode, FALSE, &timeout);
    if (status == STATUS_TIMEOUT) {
        PN_WARN("L2CAP connect to PSM 0x%04X timed out", Psm);
        WdfRequestCancelSentRequest(Ch->OpenCloseRequest);
        KeWaitForSingleObject(&Ch->ConnectedEvent, Executive, KernelMode, FALSE, NULL);
    }
    return Ch->OpenStatus;
}

NTSTATUS PnChannelAccept(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ PINDICATION_PARAMETERS Params, _In_ USHORT PreferredMtu)
{
    PPN_BTH_LINK link = Ch->Link;
    KIRQL irql;
    KeAcquireSpinLock(&Ch->Lock, &irql);
    if (Ch->State != PnChannelFree && Ch->State != PnChannelDisconnected) {
        KeReleaseSpinLock(&Ch->Lock, irql);
        return STATUS_INVALID_DEVICE_STATE;
    }
    Ch->State = PnChannelConnecting;
    Ch->RemoteInitiated = TRUE;
    Ch->OpenStatus = STATUS_PENDING;
    Ch->Generation++;
    Ch->ReadersStarted = FALSE;
    KeClearEvent(&Ch->ConnectedEvent);
    KeClearEvent(&Ch->DisconnectedEvent);
    KeReleaseSpinLock(&Ch->Lock, irql);

    struct _BRB_L2CA_OPEN_ENHANCED_CHANNEL *brb = &Ch->OpenBrb;
    link->Ddi.BthInitializeBrb((PBRB)brb, BRB_L2CA_OPEN_ENHANCED_CHANNEL_RESPONSE);
    brb->Hdr.ClientContext[0] = Ch;
    brb->BtAddress = Params->BtAddress;
    brb->Psm = Params->Parameters.Connect.Request.PSM;
    brb->ChannelHandle = Params->ConnectionHandle;
    brb->Response = CONNECT_RSP_RESULT_SUCCESS;
    PnFillOpenConfig(brb, PreferredMtu);
    brb->CallbackContext = Ch;
    brb->ReferenceObject = link->Fdo;

    NTSTATUS status = PnSendBrbAsync(link->IoTarget, Ch->OpenCloseRequest, &Ch->OpenCloseBrbMemory, (PBRB)brb, sizeof(*brb), PnOpenCompletion, Ch);
    if (!NT_SUCCESS(status)) {
        KeAcquireSpinLock(&Ch->Lock, &irql);
        Ch->State = PnChannelDisconnected;
        Ch->OpenStatus = status;
        KeSetEvent(&Ch->DisconnectedEvent, 0, FALSE);
        KeSetEvent(&Ch->ConnectedEvent, 0, FALSE);
        KeReleaseSpinLock(&Ch->Lock, irql);
    }
    return status;
}

static VOID PnCloseCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
                              _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Request); UNREFERENCED_PARAMETER(Target);
    PPN_L2CAP_CHANNEL ch = (PPN_L2CAP_CHANNEL)Context;
    KIRQL irql;
    if (!NT_SUCCESS(Params->IoStatus.Status)) PN_WARN("%s channel close completed with 0x%08x", ch->IsMedia ? "media" : "signalling", Params->IoStatus.Status);
    KeAcquireSpinLock(&ch->Lock, &irql);
    ch->State = PnChannelDisconnected;
    ch->Handle = NULL;
    KeSetEvent(&ch->DisconnectedEvent, 0, FALSE);
    KeReleaseSpinLock(&ch->Lock, irql);
}

static BOOLEAN PnChannelDisconnectEx(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ BOOLEAN CheckGen, _In_ ULONG Generation)
{
    PPN_BTH_LINK link = Ch->Link;
    KIRQL irql;
    if (!link) return FALSE;   /* never initialized */
    KeAcquireSpinLock(&Ch->Lock, &irql);
    if ((CheckGen && Ch->Generation != Generation) || (Ch->State != PnChannelConnecting && Ch->State != PnChannelConnected)) {
        KeReleaseSpinLock(&Ch->Lock, irql);
        return FALSE;
    }
    BOOLEAN wasConnected = Ch->State == PnChannelConnected;
    Ch->State = PnChannelDisconnecting;     /* while Connecting: the open completion closes it */
    KeClearEvent(&Ch->DisconnectedEvent);
    KeReleaseSpinLock(&Ch->Lock, irql);

    /* resubmits now see a channel that is not Connected and end their chains */
    PnChannelStopReaders(Ch);
    if (!wasConnected) return TRUE;

    struct _BRB_L2CA_CLOSE_CHANNEL *brb = &Ch->CloseBrb;
    link->Ddi.BthInitializeBrb((PBRB)brb, BRB_L2CA_CLOSE_CHANNEL);
    brb->BtAddress = Ch->RemoteAddress;
    brb->ChannelHandle = Ch->Handle;
    NTSTATUS status = PnSendBrbAsync(link->IoTarget, Ch->CloseRequest, &Ch->CloseBrbMemory, (PBRB)brb, sizeof(*brb), PnCloseCompletion, Ch);
    if (!NT_SUCCESS(status)) {
        /* Already gone; mark closed. */
        KeAcquireSpinLock(&Ch->Lock, &irql);
        Ch->State = PnChannelDisconnected;
        Ch->Handle = NULL;
        KeSetEvent(&Ch->DisconnectedEvent, 0, FALSE);
        KeReleaseSpinLock(&Ch->Lock, irql);
    }
    return TRUE;
}

BOOLEAN PnChannelDisconnect(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    return PnChannelDisconnectEx(Ch, FALSE, 0);
}

BOOLEAN PnChannelDisconnectGen(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ ULONG Generation)
{
    return PnChannelDisconnectEx(Ch, TRUE, Generation);
}

/* DeviceStop: an outgoing open to a speaker that is off can take the full page timeout, and
 * its caller holds StateMutex. Cancelling it lets the caller return early. */
VOID PnChannelCancelConnect(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    KIRQL irql;
    BOOLEAN cancel = FALSE;
    if (!Ch->Link || !Ch->OpenCloseRequest) return;
    KeAcquireSpinLock(&Ch->Lock, &irql);
    cancel = (Ch->State == PnChannelConnecting && !Ch->RemoteInitiated);
    KeReleaseSpinLock(&Ch->Lock, irql);
    if (cancel) WdfRequestCancelSentRequest(Ch->OpenCloseRequest);
}

/* Closes the current connection and waits until it is closed. An accept of a new connection
 * clears DisconnectedEvent, so the wait is for this generation: it ends once the channel is
 * closed or a newer connection has replaced it. */
VOID PnChannelDisconnectSync(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    PAGED_CODE();
    KIRQL irql;
    if (!Ch->Link) return;
    KeAcquireSpinLock(&Ch->Lock, &irql);
    ULONG gen = Ch->Generation;
    KeReleaseSpinLock(&Ch->Lock, irql);
    PnChannelDisconnectGen(Ch, gen);
    for (;;) {
        LARGE_INTEGER t;
        t.QuadPart = -PN_HNS_PER_MS * 100;
        if (KeWaitForSingleObject(&Ch->DisconnectedEvent, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS) return;
        KeAcquireSpinLock(&Ch->Lock, &irql);
        BOOLEAN done = Ch->Generation != gen || Ch->State == PnChannelFree || Ch->State == PnChannelDisconnected;
        KeReleaseSpinLock(&Ch->Lock, irql);
        if (done) return;
    }
}

/* PASSIVE. Starts the reader chains once per connection. A chain of the previous
 * connection may still be ending (cancelled read, queued resubmit DPC): wait briefly for
 * it, and skip a reader whose request is still owned by the stack rather than resend it. */
NTSTATUS PnChannelStartReaders(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    PAGED_CODE();
    KIRQL irql;
    ULONG gen;
    if (!Ch->Link) return STATUS_INVALID_DEVICE_STATE;
    /* Callers (event worker, AvdtpConnectSignalling, AvdtpOpen) can overlap for different
     * generations; only one may be claiming readers at a time. Innermost mutex: nothing
     * below takes another one (OnClosed only queues an event). */
    PnMutexAcquire(&Ch->StartMutex);
    KeAcquireSpinLock(&Ch->Lock, &irql);
    BOOLEAN go = Ch->State == PnChannelConnected && !Ch->ReadersStarted;
    if (go) Ch->ReadersStarted = TRUE;
    gen = Ch->Generation;
    KeReleaseSpinLock(&Ch->Lock, irql);
    for (ULONG i = 0; go && i < Ch->ReadersInitialized; i++) {
        PPN_L2CAP_READER r = &Ch->Readers[i];
        LARGE_INTEGER t;
        t.QuadPart = -PN_HNS_PER_MS * 500;
        /* not signalled: a read or a resubmit DPC of the old chain is still pending */
        NTSTATUS wait = KeWaitForSingleObject(&r->StoppedEvent, Executive, KernelMode, FALSE, &t);
        KeAcquireSpinLock(&Ch->Lock, &irql);
        BOOLEAN live = Ch->Generation == gen && Ch->State == PnChannelConnected;
        BOOLEAN ok = live && wait == STATUS_SUCCESS && !r->Outstanding;
        if (ok) { r->Generation = gen; r->Stopping = 0; }
        KeReleaseSpinLock(&Ch->Lock, irql);
        if (!live) break;   /* this connection is gone; a newer one starts its own readers */
        if (!ok) { PN_WARN("%s reader %u still busy from the previous connection; skipped", Ch->IsMedia ? "media" : "signalling", i); continue; }
        PnReaderSubmit(r);
    }
    PnMutexRelease(&Ch->StartMutex);
    return STATUS_SUCCESS;
}

VOID PnChannelStopReaders(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    for (ULONG i = 0; i < Ch->ReadersInitialized; i++) {
        PPN_L2CAP_READER r = &Ch->Readers[i];
        KIRQL irql;
        KeAcquireSpinLock(&r->SendLock, &irql);
        InterlockedExchange(&r->Stopping, 1);
        if (r->Outstanding && r->Request) WdfRequestCancelSentRequest(r->Request);
        KeReleaseSpinLock(&r->SendLock, irql);
    }
}

PPN_L2CAP_WRITE PnChannelAcquireWrite(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    KIRQL irql;
    PPN_L2CAP_WRITE w = NULL;
    KeAcquireSpinLock(&Ch->Lock, &irql);
    if (Ch->State == PnChannelConnected && !IsListEmpty(&Ch->FreeWrites)) {
        PLIST_ENTRY e = RemoveHeadList(&Ch->FreeWrites);
        w = CONTAINING_RECORD(e, PN_L2CAP_WRITE, Link);
        InsertTailList(&Ch->BusyWrites, &w->Link);
        w->Length = 0;
    }
    KeReleaseSpinLock(&Ch->Lock, irql);
    return w;
}

VOID PnChannelReleaseWrite(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ PPN_L2CAP_WRITE W)
{
    KIRQL irql;
    KeAcquireSpinLock(&Ch->Lock, &irql);
    RemoveEntryList(&W->Link);
    InsertTailList(&Ch->FreeWrites, &W->Link);
    KeReleaseSpinLock(&Ch->Lock, irql);
}

NTSTATUS PnChannelSubmitWrite(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ PPN_L2CAP_WRITE W)
{
    PPN_BTH_LINK link = Ch->Link;
    WDF_REQUEST_REUSE_PARAMS reuse;
    if (Ch->State != PnChannelConnected) { PnChannelReleaseWrite(Ch, W); return STATUS_CONNECTION_DISCONNECTED; }
    if (W->Length == 0 || W->Length > W->Capacity) { PnChannelReleaseWrite(Ch, W); return STATUS_INVALID_PARAMETER; }

    link->Ddi.BthReuseBrb((PBRB)&W->Brb, BRB_L2CA_ACL_TRANSFER);
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_UNSUCCESSFUL);
    NTSTATUS status = WdfRequestReuse(W->Request, &reuse);
    if (!NT_SUCCESS(status)) { PnChannelReleaseWrite(Ch, W); return status; }
    W->Brb.BtAddress = Ch->RemoteAddress;
    W->Brb.ChannelHandle = Ch->Handle;
    W->Brb.TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
    W->Brb.Buffer = W->Buffer;
    W->Brb.BufferMDL = NULL;
    W->Brb.BufferSize = W->Length;
    W->SubmitTimeHns = PnNowHns();
    status = PnFormatRequestWithBrb(link->IoTarget, W->Request, &W->BrbMemory, (PBRB)&W->Brb, sizeof(W->Brb));
    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(W->Request, PnWriteCompletion, W);
        InterlockedIncrement(&Ch->WritesInFlight);
        InterlockedExchange(&W->Outstanding, 1);
        if (!WdfRequestSend(W->Request, link->IoTarget, NULL)) {
            InterlockedExchange(&W->Outstanding, 0);
            status = WdfRequestGetStatus(W->Request);
            InterlockedDecrement(&Ch->WritesInFlight);
        } else {
            return STATUS_SUCCESS;
        }
    }
    PnChannelReleaseWrite(Ch, W);
    return status;
}

NTSTATUS PnChannelWriteCopy(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_reads_bytes_(Length) const UCHAR *Data, _In_ ULONG Length)
{
    PPN_L2CAP_WRITE w = PnChannelAcquireWrite(Ch);
    if (!w) return STATUS_DEVICE_BUSY;
    if (Length > w->Capacity) { PnChannelReleaseWrite(Ch, w); return STATUS_BUFFER_OVERFLOW; }
    RtlCopyMemory(w->Buffer, Data, Length);
    w->Length = Length;
    return PnChannelSubmitWrite(Ch, w);
}
