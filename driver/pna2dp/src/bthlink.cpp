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

static NTSTATUS PnFormatRequestWithBrb(_In_ WDFIOTARGET Target, _In_ WDFREQUEST Request, _In_ PBRB Brb, _In_ size_t BrbSize)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDFMEMORY mem = NULL;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Request;
    NTSTATUS status = WdfMemoryCreatePreallocated(&attr, Brb, BrbSize, &mem);
    if (!NT_SUCCESS(status)) return status;
    return WdfIoTargetFormatRequestForInternalIoctlOthers(Target, Request, IOCTL_INTERNAL_BTH_SUBMIT_BRB,
                                                         mem, NULL, NULL, NULL, NULL, NULL);
}

static NTSTATUS PnSendBrbAsync(_In_ WDFIOTARGET Target, _In_ WDFREQUEST Request, _In_ PBRB Brb, _In_ size_t BrbSize,
                               _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion, _In_opt_ WDFCONTEXT Context)
{
    WDF_REQUEST_REUSE_PARAMS reuse;
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
    NTSTATUS status = WdfRequestReuse(Request, &reuse);
    if (!NT_SUCCESS(status)) return status;
    status = PnFormatRequestWithBrb(Target, Request, Brb, BrbSize);
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
    ExAcquireFastMutex(&Link->SyncRequestMutex);
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
    NTSTATUS status = WdfRequestReuse(Link->SyncRequest, &reuse);
    if (NT_SUCCESS(status)) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, Brb, BrbSize);
        status = WdfIoTargetSendInternalIoctlOthersSynchronously(Link->IoTarget, Link->SyncRequest,
                                                                 IOCTL_INTERNAL_BTH_SUBMIT_BRB, &desc, NULL, NULL, NULL, NULL);
    }
    ExReleaseFastMutex(&Link->SyncRequestMutex);
    return status;
}

/* ------------------------------------------------------------------------- */
/* Query the profile driver DDI with a PnP IRP (we are not a WDF FDO)          */
/* ------------------------------------------------------------------------- */

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
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* SDP: read the remote AudioSink / AudioSource record                          */
/* ------------------------------------------------------------------------- */

static NTSTATUS PnSdpParseRecord(_In_ PPN_BTH_LINK Link, _In_ PBTH_SDP_STREAM_RESPONSE Rec)
{
    PAGED_CODE();
    BTHDDI_SDP_PARSE_INTERFACE parse;
    RtlZeroMemory(&parse, sizeof(parse));
    NTSTATUS status = PnQueryInterface(Link->Pdo, &GUID_BTHDDI_SDP_PARSE_INTERFACE, sizeof(parse),
                                       BTHDDI_SDP_PARSE_INTERFACE_VERSION_FOR_QI, (PINTERFACE)&parse);
    if (!NT_SUCCESS(status)) return status;

    PUCHAR next = NULL;
    ULONG nextSize = 0;
    parse.SdpGetNextElement(&Rec->response[0], Rec->responseSize, NULL, &next, &nextSize);
    if (nextSize == 0) return STATUS_DEVICE_DATA_ERROR;

    PSDP_TREE_ROOT_NODE tree = NULL;
    status = parse.SdpConvertStreamToTree(next, nextSize, &tree, PNA2DP_TAG_SDP);
    if (!NT_SUCCESS(status)) return status;

    /* Profile descriptor list: seq( seq( uuid16 0x110D, uint16 version ) ) */
    PSDP_NODE node = NULL;
    if (NT_SUCCESS(parse.SdpFindAttributeInTree(tree, SDP_ATTRIB_PROFILE_DESCRIPTOR_LIST, &node)) && node &&
        node->hdr.Type == SDP_TYPE_SEQUENCE && node->u.sequence.Link.Flink) {
        PSDP_NODE inner = CONTAINING_RECORD(node->u.sequence.Link.Flink, SDP_NODE, hdr.Link);
        if (inner->hdr.Type == SDP_TYPE_SEQUENCE && inner->u.sequence.Link.Flink) {
            PSDP_NODE uuid = CONTAINING_RECORD(inner->u.sequence.Link.Flink, SDP_NODE, hdr.Link);
            if (uuid->hdr.Link.Flink) {
                PSDP_NODE ver = CONTAINING_RECORD(uuid->hdr.Link.Flink, SDP_NODE, hdr.Link);
                if (ver->hdr.SpecificType == SDP_ST_UINT16) Link->RemoteAvdtpVersion = ver->u.uint16;
            }
        }
    }
    /* SupportedFeatures 0x0311 */
    node = NULL;
    if (NT_SUCCESS(parse.SdpFindAttributeInTree(tree, 0x0311, &node)) && node && node->hdr.SpecificType == SDP_ST_UINT16) {
        Link->RemoteSupportedFeatures = node->u.uint16;
    }
    parse.SdpFreeTree(tree);
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
                    status = PnSdpParseRecord(Link, rec);
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
    switch (Indication) {
    case IndicationRemoteConnect:
        if (Params->BtAddress != link->RemoteAddress) {
            /* Not our device: let BthPort time it out; a reject needs a channel object. */
            PN_WARN("connect indication from foreign address %012llX ignored", Params->BtAddress);
            PnLinkRejectIncoming(link, Params);
            return;
        }
        if (link->OnIncoming) link->OnIncoming(link->CallbackContext, Params);
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

VOID PnLinkUnregisterServer(_Inout_ PPN_BTH_LINK Link)
{
    PAGED_CODE();
    if (!Link->DdiValid) return;
    if (Link->ServerHandle) {
        struct _BRB_L2CA_UNREGISTER_SERVER brb;
        Link->Ddi.BthInitializeBrb((PBRB)&brb, BRB_L2CA_UNREGISTER_SERVER);
        brb.BtAddress = Link->RemoteAddress;
        brb.Psm = 0;
        brb.ServerHandle = Link->ServerHandle;
        NTSTATUS status = PnLinkSendBrbSync(Link, (PBRB)&brb, sizeof(brb));
        if (!NT_SUCCESS(status)) PN_WARN("BRB_L2CA_UNREGISTER_SERVER failed 0x%08x", status);
        Link->ServerHandle = NULL;
    }
    if (Link->Psm) {
        Link->Ddi.BthInitializeBrb((PBRB)&Link->PsmBrb, BRB_UNREGISTER_PSM);
        Link->PsmBrb.Psm = Link->Psm;
        PnLinkSendBrbSync(Link, (PBRB)&Link->PsmBrb, sizeof(Link->PsmBrb));
        Link->Psm = 0;
    }
    Link->OnIncoming = NULL;
}

/* Reject with a one-shot BRB that frees itself on completion. */
typedef struct _PN_REJECT_CTX {
    PPN_BTH_LINK Link;
    PBRB         Brb;
} PN_REJECT_CTX, *PPN_REJECT_CTX;
WDF_DECLARE_CONTEXT_TYPE(PN_REJECT_CTX);

static VOID PnRejectCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
                               _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);
    UNREFERENCED_PARAMETER(Context);
    PPN_REJECT_CTX ctx = WdfObjectGet_PN_REJECT_CTX(Request);
    if (ctx && ctx->Brb && ctx->Link) ctx->Link->Ddi.BthFreeBrb(ctx->Brb);
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
    ctx->Link = Link;
    ctx->Brb = (PBRB)brb;
    brb->BtAddress = Params->BtAddress;
    brb->Psm = Params->Parameters.Connect.Request.PSM;
    brb->ChannelHandle = Params->ConnectionHandle;
    brb->Response = CONNECT_RSP_RESULT_NO_RESOURCES;
    brb->ChannelFlags = CF_ROLE_EITHER;
    brb->ReferenceObject = Link->Fdo;
    status = PnSendBrbAsync(Link->IoTarget, request, (PBRB)brb, sizeof(*brb), PnRejectCompletion, Link);
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
        /* Radio going away: drop our file object reference so the removal can proceed. */
        PN_WARN("radio removal notification");
        PnLinkUnregisterRadioEvents(link);
    }
    return STATUS_SUCCESS;
}

NTSTATUS PnLinkRegisterRadioEvents(_Inout_ PPN_BTH_LINK Link, _In_ PFN_PN_LINK_SCO OnSco, _In_ PFN_PN_LINK_ACL OnAcl, _In_ PVOID Context)
{
    PAGED_CODE();
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
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, list);
    PFILE_OBJECT file = NULL;
    PDEVICE_OBJECT dev = NULL;
    status = IoGetDeviceObjectPointer(&name, FILE_READ_DATA, &file, &dev);
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
    if (NT_SUCCESS(status)) {
        if (ch->OnReceive && r->Brb.BufferSize) {
            ch->OnReceive(ch->CallbackContext, ch, r->Buffer, r->Brb.BufferSize);
        }
        KeInsertQueueDpc(&r->ResubmitDpc, r, NULL);
    } else {
        if (status != STATUS_CANCELLED) {
            PN_WARN("L2CAP read failed 0x%08x on %s channel", status, ch->IsMedia ? "media" : "signalling");
            if (ch->OnClosed) ch->OnClosed(ch->CallbackContext, ch, status);
        }
        KeSetEvent(&r->StoppedEvent, 0, FALSE);
    }
}

static VOID PnReaderSubmit(_In_ PPN_L2CAP_READER R)
{
    PPN_L2CAP_CHANNEL ch = R->Channel;
    PPN_BTH_LINK link = ch->Link;
    WDF_REQUEST_REUSE_PARAMS reuse;

    if (R->Stopping || ch->State != PnChannelConnected) {
        KeSetEvent(&R->StoppedEvent, 0, FALSE);
        return;
    }
    link->Ddi.BthReuseBrb((PBRB)&R->Brb, BRB_L2CA_ACL_TRANSFER);
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_UNSUCCESSFUL);
    WdfRequestReuse(R->Request, &reuse);
    R->Brb.BtAddress = ch->RemoteAddress;
    R->Brb.ChannelHandle = ch->Handle;
    R->Brb.TransferFlags = ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK;
    R->Brb.Buffer = R->Buffer;
    R->Brb.BufferMDL = NULL;
    R->Brb.BufferSize = R->BufferSize;
    NTSTATUS status = PnFormatRequestWithBrb(link->IoTarget, R->Request, (PBRB)&R->Brb, sizeof(R->Brb));
    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(R->Request, PnReaderCompletion, R);
        KeClearEvent(&R->StoppedEvent);
        if (!WdfRequestSend(R->Request, link->IoTarget, NULL)) {
            status = WdfRequestGetStatus(R->Request);
        } else {
            return;
        }
    }
    PN_WARN("reader submit failed 0x%08x", status);
    if (ch->OnClosed) ch->OnClosed(ch->CallbackContext, ch, status);
    KeSetEvent(&R->StoppedEvent, 0, FALSE);
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
    KeAcquireSpinLock(&ch->Lock, &irql);
    RemoveEntryList(&w->Link);
    InsertTailList(&ch->FreeWrites, &w->Link);
    InterlockedDecrement(&ch->WritesInFlight);
    KeReleaseSpinLock(&ch->Lock, irql);
    if (ch->OnWriteDone) ch->OnWriteDone(ch->CallbackContext, ch, status, bytes, submitted);
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
    KeInitializeEvent(&Ch->DisconnectedEvent, NotificationEvent, TRUE);
    KeInitializeEvent(&Ch->ConnectedEvent, NotificationEvent, FALSE);
    InitializeListHead(&Ch->FreeWrites);
    InitializeListHead(&Ch->BusyWrites);

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Link->WdfDevice;
    status = WdfRequestCreate(&attr, Link->IoTarget, &Ch->OpenCloseRequest);
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
    for (ULONG i = 0; i < Ch->ReadersInitialized; i++) {
        KeWaitForSingleObject(&Ch->Readers[i].StoppedEvent, Executive, KernelMode, FALSE, NULL);
        if (Ch->Readers[i].Request) { WdfObjectDelete(Ch->Readers[i].Request); Ch->Readers[i].Request = NULL; }
    }
    /* Outstanding writes complete with STATUS_CANCELLED/DISCONNECTED once the channel is closed. */
    LARGE_INTEGER wait;
    wait.QuadPart = -PN_HNS_PER_MS * 20;
    for (int spins = 0; spins < 100 && Ch->WritesInFlight > 0; spins++) {
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
    }
    for (ULONG i = 0; i < PN_L2CAP_MAX_WRITES; i++) {
        if (Ch->Writes[i].Request) { WdfObjectDelete(Ch->Writes[i].Request); Ch->Writes[i].Request = NULL; }
    }
    if (Ch->OpenCloseRequest) { WdfObjectDelete(Ch->OpenCloseRequest); Ch->OpenCloseRequest = NULL; }
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
    UNREFERENCED_PARAMETER(Params);
    PPN_L2CAP_CHANNEL ch = (PPN_L2CAP_CHANNEL)Context;
    switch (Indication) {
    case IndicationRemoteDisconnect:
        PN_INFO("remote closed %s channel", ch->IsMedia ? "media" : "signalling");
        PnChannelDisconnect(ch);           /* complete the teardown from our side */
        if (ch->OnClosed) ch->OnClosed(ch->CallbackContext, ch, STATUS_CONNECTION_DISCONNECTED);
        break;
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
        KeSetEvent(&ch->DisconnectedEvent, 0, FALSE);
    }
    ch->OpenStatus = status;
    KeReleaseSpinLock(&ch->Lock, irql);
    PN_INFO("%s channel open: 0x%08x mtu out %u in %u", ch->IsMedia ? "media" : "signalling", status, ch->OutMtu, ch->InMtu);
    KeSetEvent(&ch->ConnectedEvent, 0, FALSE);
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

    NTSTATUS status = PnSendBrbAsync(link->IoTarget, Ch->OpenCloseRequest, (PBRB)brb, sizeof(*brb), PnOpenCompletion, Ch);
    if (!NT_SUCCESS(status)) {
        KeAcquireSpinLock(&Ch->Lock, &irql);
        Ch->State = PnChannelDisconnected;
        KeSetEvent(&Ch->DisconnectedEvent, 0, FALSE);
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

    NTSTATUS status = PnSendBrbAsync(link->IoTarget, Ch->OpenCloseRequest, (PBRB)brb, sizeof(*brb), PnOpenCompletion, Ch);
    if (!NT_SUCCESS(status)) {
        KeAcquireSpinLock(&Ch->Lock, &irql);
        Ch->State = PnChannelDisconnected;
        KeSetEvent(&Ch->DisconnectedEvent, 0, FALSE);
        KeReleaseSpinLock(&Ch->Lock, irql);
    }
    return status;
}

static VOID PnCloseCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
                              _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Request); UNREFERENCED_PARAMETER(Target); UNREFERENCED_PARAMETER(Params);
    PPN_L2CAP_CHANNEL ch = (PPN_L2CAP_CHANNEL)Context;
    KIRQL irql;
    KeAcquireSpinLock(&ch->Lock, &irql);
    ch->State = PnChannelDisconnected;
    ch->Handle = NULL;
    KeReleaseSpinLock(&ch->Lock, irql);
    KeSetEvent(&ch->DisconnectedEvent, 0, FALSE);
}

BOOLEAN PnChannelDisconnect(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    PPN_BTH_LINK link = Ch->Link;
    KIRQL irql;
    PnChannelStopReaders(Ch);
    KeAcquireSpinLock(&Ch->Lock, &irql);
    if (Ch->State == PnChannelConnecting) {
        Ch->State = PnChannelDisconnecting;     /* close after the open completes */
        KeClearEvent(&Ch->DisconnectedEvent);
        KeReleaseSpinLock(&Ch->Lock, irql);
        return TRUE;
    }
    if (Ch->State != PnChannelConnected) {
        KeReleaseSpinLock(&Ch->Lock, irql);
        return FALSE;
    }
    Ch->State = PnChannelDisconnecting;
    KeClearEvent(&Ch->DisconnectedEvent);
    KeReleaseSpinLock(&Ch->Lock, irql);

    struct _BRB_L2CA_CLOSE_CHANNEL *brb = (struct _BRB_L2CA_CLOSE_CHANNEL *)&Ch->OpenBrb;
    link->Ddi.BthReuseBrb((PBRB)brb, BRB_L2CA_CLOSE_CHANNEL);
    brb->BtAddress = Ch->RemoteAddress;
    brb->ChannelHandle = Ch->Handle;
    NTSTATUS status = PnSendBrbAsync(link->IoTarget, Ch->OpenCloseRequest, (PBRB)brb, sizeof(*brb), PnCloseCompletion, Ch);
    if (!NT_SUCCESS(status)) {
        /* Already gone; mark closed. */
        KeAcquireSpinLock(&Ch->Lock, &irql);
        Ch->State = PnChannelDisconnected;
        Ch->Handle = NULL;
        KeReleaseSpinLock(&Ch->Lock, irql);
        KeSetEvent(&Ch->DisconnectedEvent, 0, FALSE);
    }
    return TRUE;
}

VOID PnChannelDisconnectSync(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    PAGED_CODE();
    PnChannelDisconnect(Ch);
    KeWaitForSingleObject(&Ch->DisconnectedEvent, Executive, KernelMode, FALSE, NULL);
}

NTSTATUS PnChannelStartReaders(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    for (ULONG i = 0; i < Ch->ReadersInitialized; i++) {
        Ch->Readers[i].Stopping = 0;
        PnReaderSubmit(&Ch->Readers[i]);
    }
    return STATUS_SUCCESS;
}

VOID PnChannelStopReaders(_Inout_ PPN_L2CAP_CHANNEL Ch)
{
    for (ULONG i = 0; i < Ch->ReadersInitialized; i++) {
        InterlockedExchange(&Ch->Readers[i].Stopping, 1);
        if (Ch->Readers[i].Request) WdfRequestCancelSentRequest(Ch->Readers[i].Request);
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
    WdfRequestReuse(W->Request, &reuse);
    W->Brb.BtAddress = Ch->RemoteAddress;
    W->Brb.ChannelHandle = Ch->Handle;
    W->Brb.TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
    W->Brb.Buffer = W->Buffer;
    W->Brb.BufferMDL = NULL;
    W->Brb.BufferSize = W->Length;
    W->SubmitTimeHns = PnNowHns();
    NTSTATUS status = PnFormatRequestWithBrb(link->IoTarget, W->Request, (PBRB)&W->Brb, sizeof(W->Brb));
    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(W->Request, PnWriteCompletion, W);
        InterlockedIncrement(&Ch->WritesInFlight);
        if (!WdfRequestSend(W->Request, link->IoTarget, NULL)) {
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
