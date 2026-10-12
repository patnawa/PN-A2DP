/*
 * bthlink.h - Bluetooth link layer for the A2DP profile driver.
 *
 * Wraps the BthPort profile-driver DDI (BRBs) behind a small, testable API:
 *   - PN_BTH_LINK      : per-device handle to the BthEnum PDO (I/O target, DDI table,
 *                        local/remote addresses, PSM/server registration, PnP events)
 *   - PN_L2CAP_CHANNEL : one L2CAP channel (signalling or media) with continuous readers
 *                        and asynchronous writes
 *   - SDP helpers      : query the remote AudioSink/AudioSource record
 *
 * All BRBs are issued as KMDF requests on the default I/O target of a WDF miniport
 * device created over the PortCls FDO.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define PN_L2CAP_READERS        1       /* one chain preserves signalling packet order */
#define PN_L2CAP_MAX_WRITES     8       /* bounded outstanding writes per channel */
#define PN_L2CAP_SIG_MTU        672
#define PN_L2CAP_MEDIA_MTU_PREF 1024    /* preferred incoming/outgoing media MTU */

typedef enum _PN_CHANNEL_STATE {
    PnChannelFree = 0,
    PnChannelConnecting,
    PnChannelConnected,
    PnChannelDisconnecting,
    PnChannelDisconnected
} PN_CHANNEL_STATE;

typedef VOID (*PFN_PN_L2CAP_RECEIVE)(
    _In_ PVOID Context,
    _In_ PPN_L2CAP_CHANNEL Channel,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length);

typedef VOID (*PFN_PN_L2CAP_EVENT)(
    _In_ PVOID Context,
    _In_ PPN_L2CAP_CHANNEL Channel,
    _In_ NTSTATUS Status,            /* channel went down (remote close or failure) */
    _In_ ULONG Generation);          /* the connection it concerns (see PN_L2CAP_CHANNEL) */

typedef VOID (*PFN_PN_L2CAP_WRITE_DONE)(
    _In_ PVOID Context,
    _In_ PPN_L2CAP_CHANNEL Channel,
    _In_ NTSTATUS Status,
    _In_ ULONG Bytes,
    _In_ LONGLONG SubmitTimeHns);

typedef struct _PN_L2CAP_READER {
    struct _BRB_L2CA_ACL_TRANSFER Brb;
    WDFREQUEST   Request;
    WDFMEMORY    Memory;
    WDFMEMORY    BrbMemory;     /* reused for every send of Request */
    PUCHAR       Buffer;
    ULONG        BufferSize;
    KDPC         ResubmitDpc;
    KSPIN_LOCK   SendLock;       /* prevents cancellation from racing the send itself */
    LONG         Stopping;
    LONG         Outstanding;   /* Request is sent and not completed */
    ULONG        Generation;    /* connection this reader chain belongs to */
    KEVENT       StoppedEvent;
    PPN_L2CAP_CHANNEL Channel;
} PN_L2CAP_READER, *PPN_L2CAP_READER;

typedef struct _PN_L2CAP_WRITE {
    LIST_ENTRY   Link;
    struct _BRB_L2CA_ACL_TRANSFER Brb;
    WDFREQUEST   Request;
    WDFMEMORY    Memory;        /* preallocated, wraps Buffer */
    WDFMEMORY    BrbMemory;     /* reused for every send of Request */
    LONG         Outstanding;
    PUCHAR       Buffer;
    ULONG        Capacity;
    ULONG        Length;
    LONGLONG     SubmitTimeHns;
    PPN_L2CAP_CHANNEL Channel;
} PN_L2CAP_WRITE, *PPN_L2CAP_WRITE;

typedef struct PN_L2CAP_CHANNEL {
    struct _PN_BTH_LINK   *Link;
    PN_CHANNEL_STATE       State;
    KSPIN_LOCK             Lock;
    L2CAP_CHANNEL_HANDLE   Handle;
    BTH_ADDR               RemoteAddress;
    USHORT                 OutMtu;
    USHORT                 InMtu;
    BOOLEAN                RemoteInitiated;
    BOOLEAN                IsMedia;             /* second channel = media transport */

    struct _BRB_L2CA_OPEN_ENHANCED_CHANNEL OpenBrb;   /* open / accept response */
    WDFREQUEST             OpenCloseRequest;    /* carries opens only (name kept for history) */
    WDFMEMORY              OpenCloseBrbMemory;
    struct _BRB_L2CA_CLOSE_CHANNEL CloseBrb;    /* own request: a cancel of an open never hits a close */
    WDFREQUEST             CloseRequest;
    WDFMEMORY              CloseBrbMemory;
    FAST_MUTEX             StartMutex;          /* serializes PnChannelStartReaders (innermost mutex) */
    KEVENT                 DisconnectedEvent;   /* set when fully closed */
    KEVENT                 ConnectedEvent;      /* set when open completes (any status) */
    NTSTATUS               OpenStatus;
    ULONG                  Generation;          /* +1 per connect/accept; tags events of one connection */
    BOOLEAN                ReadersStarted;      /* for this generation (under Lock) */

    PN_L2CAP_READER        Readers[PN_L2CAP_READERS];
    ULONG                  ReadersInitialized;

    LIST_ENTRY             FreeWrites;
    LIST_ENTRY             BusyWrites;
    PN_L2CAP_WRITE         Writes[PN_L2CAP_MAX_WRITES];
    LONG                   WritesInFlight;
    ULONG                  WriteCapacity;

    PFN_PN_L2CAP_RECEIVE     OnReceive;
    PFN_PN_L2CAP_EVENT       OnClosed;
    PFN_PN_L2CAP_WRITE_DONE  OnWriteDone;
    PVOID                    CallbackContext;
} PN_L2CAP_CHANNEL;

typedef VOID (*PFN_PN_LINK_INCOMING)(
    _In_ PVOID Context,
    _In_ PINDICATION_PARAMETERS Params);   /* DISPATCH_LEVEL; must respond */

typedef VOID (*PFN_PN_LINK_SCO)(
    _In_ PVOID Context,
    _In_ BOOLEAN ScoActive);               /* PASSIVE (PnP notification) */

typedef VOID (*PFN_PN_LINK_ACL)(
    _In_ PVOID Context,
    _In_ BOOLEAN AclConnected);            /* PASSIVE */

typedef struct _PN_BTH_LINK {
    WDFDEVICE                    WdfDevice;       /* miniport device over the FDO */
    WDFIOTARGET                  IoTarget;        /* -> BthEnum PDO */
    PDEVICE_OBJECT               Fdo;
    PDEVICE_OBJECT               Pdo;
    BTH_PROFILE_DRIVER_INTERFACE Ddi;
    BOOLEAN                      DdiValid;
    BTH_ADDR                     LocalAddress;
    BTH_ADDR                     RemoteAddress;
    BTH_COD                      RemoteCod;
    CHAR                         RemoteName[BTH_MAX_NAME_SIZE];
    WCHAR                        RemoteNameW[BTH_MAX_NAME_SIZE];
    BTH_HOST_FEATURE_MASK        HostFeatures;
    BTH_LOCAL_RADIO_INFO          RadioInfo;        /* cached for the actual local address */
    NTSTATUS                     RadioQueryStatus;
    BOOLEAN                      RadioInfoValid;
    WDFREQUEST                   SyncRequest;     /* serialized init-time requests */
    FAST_MUTEX                   SyncRequestMutex;

    /* Incoming AVDTP (PSM 0x19) server */
    USHORT                       Psm;
    L2CAP_SERVER_HANDLE          ServerHandle;
    struct _BRB_L2CA_REGISTER_SERVER RegBrb;
    struct _BRB_PSM              PsmBrb;
    PFN_PN_LINK_INCOMING         OnIncoming;
    PFN_PN_LINK_SCO              OnSco;
    PFN_PN_LINK_ACL              OnAcl;
    PVOID                        CallbackContext;

    /* Radio (BthPort) interface for HCI/L2CAP PnP custom events */
    PFILE_OBJECT                 RadioFileObject;
    PDEVICE_OBJECT               RadioDeviceObject;
    PVOID                        PnpNotificationEntry;
    BOOLEAN                      ScoActive;
    BOOLEAN                      AclConnected;

    /* SDP information about the remote */
    USHORT                       RemoteAvdtpVersion;   /* from AudioSink profile descriptor */
    USHORT                       RemoteSupportedFeatures;
    BOOLEAN                      RemoteIsSink;         /* role we talk to */
} PN_BTH_LINK, *PPN_BTH_LINK;

/* ---- link ------------------------------------------------------------------ */
NTSTATUS PnLinkInitialize(_Inout_ PPN_BTH_LINK Link, _In_ WDFDEVICE WdfDevice,
                          _In_ PDEVICE_OBJECT Fdo, _In_ PDEVICE_OBJECT Pdo);
VOID     PnLinkUninitialize(_Inout_ PPN_BTH_LINK Link);
NTSTATUS PnLinkQueryRemoteInfo(_Inout_ PPN_BTH_LINK Link);
NTSTATUS PnLinkQueryRemoteSdp(_Inout_ PPN_BTH_LINK Link, _In_ USHORT ServiceUuid16);
NTSTATUS PnLinkRegisterServer(_Inout_ PPN_BTH_LINK Link, _In_ PFN_PN_LINK_INCOMING OnIncoming,
                              _In_ PVOID Context);
BOOLEAN  PnLinkUnregisterServer(_Inout_ PPN_BTH_LINK Link);   /* TRUE: a registered server could not be removed */
NTSTATUS PnLinkRegisterRadioEvents(_Inout_ PPN_BTH_LINK Link, _In_ PFN_PN_LINK_SCO OnSco,
                                   _In_ PFN_PN_LINK_ACL OnAcl, _In_ PVOID Context);
VOID     PnLinkUnregisterRadioEvents(_Inout_ PPN_BTH_LINK Link);
NTSTATUS PnLinkSendBrbSync(_In_ PPN_BTH_LINK Link, _In_ PBRB Brb, _In_ ULONG BrbSize);
NTSTATUS PnLinkEnsureActive(_Inout_ PPN_BTH_LINK Link);   /* PASSIVE: leave sniff/park before streaming */

/* ---- channels ---------------------------------------------------------------- */
NTSTATUS PnChannelInitialize(_Out_ PPN_L2CAP_CHANNEL Ch, _In_ PPN_BTH_LINK Link,
                             _In_ BOOLEAN IsMedia, _In_ ULONG ReadBufferSize,
                             _In_ ULONG WriteCapacity,
                             _In_ PFN_PN_L2CAP_RECEIVE OnReceive, _In_ PFN_PN_L2CAP_EVENT OnClosed,
                             _In_opt_ PFN_PN_L2CAP_WRITE_DONE OnWriteDone, _In_ PVOID Context);
VOID     PnChannelUninitialize(_Inout_ PPN_L2CAP_CHANNEL Ch);   /* PASSIVE: waits for rundown */

/* Outgoing connect to the remote PSM. Synchronous (PASSIVE). */
NTSTATUS PnChannelConnect(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ USHORT Psm, _In_ USHORT PreferredMtu,
                          _In_ ULONG TimeoutMs);
/* Accept an incoming connection indication (DISPATCH ok). Completion sets ConnectedEvent. */
NTSTATUS PnChannelAccept(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ PINDICATION_PARAMETERS Params,
                         _In_ USHORT PreferredMtu);
/* Reject an incoming connection (no channel object needed). */
NTSTATUS PnLinkRejectIncoming(_In_ PPN_BTH_LINK Link, _In_ PINDICATION_PARAMETERS Params);
/* Close (DISPATCH ok). Returns TRUE if a close was initiated. */
BOOLEAN  PnChannelDisconnect(_Inout_ PPN_L2CAP_CHANNEL Ch);
BOOLEAN  PnChannelDisconnectGen(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ ULONG Generation);   /* only that connection */
VOID     PnChannelDisconnectSync(_Inout_ PPN_L2CAP_CHANNEL Ch);
VOID     PnChannelCancelConnect(_Inout_ PPN_L2CAP_CHANNEL Ch);   /* cut short an outgoing open in progress */
NTSTATUS PnChannelStartReaders(_Inout_ PPN_L2CAP_CHANNEL Ch);
VOID     PnChannelStopReaders(_Inout_ PPN_L2CAP_CHANNEL Ch);
BOOLEAN  PnChannelIsConnected(_In_ PPN_L2CAP_CHANNEL Ch);

/* Writes: obtain a buffer, fill it, submit. DISPATCH ok. */
PPN_L2CAP_WRITE PnChannelAcquireWrite(_Inout_ PPN_L2CAP_CHANNEL Ch);
VOID            PnChannelReleaseWrite(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ PPN_L2CAP_WRITE W);
NTSTATUS        PnChannelSubmitWrite(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_ PPN_L2CAP_WRITE W);
NTSTATUS        PnChannelWriteCopy(_Inout_ PPN_L2CAP_CHANNEL Ch, _In_reads_bytes_(Length) const UCHAR *Data,
                                   _In_ ULONG Length);   /* convenience for signalling */
LONG            PnChannelWritesInFlight(_In_ PPN_L2CAP_CHANNEL Ch);
