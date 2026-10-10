/*
 * avdtp.h - Audio/Video Distribution Transport Protocol 1.3 (Bluetooth SIG).
 *
 * Signalling messages travel on the first L2CAP channel to PSM 0x0019, media packets
 * on the second. This module implements the signalling procedures in both roles:
 *
 *   INT (initiator, our usual role as a source): DISCOVER, GET_(ALL_)CAPABILITIES,
 *   SET_CONFIGURATION, OPEN, START, SUSPEND, RECONFIGURE, CLOSE, ABORT, DELAYREPORT.
 *   ACP (acceptor): the same commands coming from the sink, plus GET_CONFIGURATION.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

/* Message types (signal header, bits 1..0 of the first octet) */
#define AVDTP_MSG_COMMAND          0x00
#define AVDTP_MSG_GENERAL_REJECT   0x01
#define AVDTP_MSG_RESPONSE_ACCEPT  0x02
#define AVDTP_MSG_RESPONSE_REJECT  0x03

/* Packet types (bits 3..2) */
#define AVDTP_PKT_SINGLE           0x00
#define AVDTP_PKT_START            0x01
#define AVDTP_PKT_CONTINUE         0x02
#define AVDTP_PKT_END              0x03

/* Signal identifiers */
#define AVDTP_DISCOVER             0x01
#define AVDTP_GET_CAPABILITIES     0x02
#define AVDTP_SET_CONFIGURATION    0x03
#define AVDTP_GET_CONFIGURATION    0x04
#define AVDTP_RECONFIGURE          0x05
#define AVDTP_OPEN                 0x06
#define AVDTP_START                0x07
#define AVDTP_CLOSE                0x08
#define AVDTP_SUSPEND              0x09
#define AVDTP_ABORT                0x0A
#define AVDTP_SECURITY_CONTROL     0x0B
#define AVDTP_GET_ALL_CAPABILITIES 0x0C
#define AVDTP_DELAYREPORT          0x0D

/* Service categories */
#define AVDTP_CAT_MEDIA_TRANSPORT     0x01
#define AVDTP_CAT_REPORTING           0x02
#define AVDTP_CAT_RECOVERY            0x03
#define AVDTP_CAT_CONTENT_PROTECTION  0x04
#define AVDTP_CAT_HEADER_COMPRESSION  0x05
#define AVDTP_CAT_MULTIPLEXING        0x06
#define AVDTP_CAT_MEDIA_CODEC         0x07
#define AVDTP_CAT_DELAY_REPORTING     0x08

/* Error codes (AVDTP 8.20.6) */
#define AVDTP_ERR_BAD_HEADER_FORMAT        0x01
#define AVDTP_ERR_BAD_LENGTH               0x11
#define AVDTP_ERR_BAD_ACP_SEID             0x12
#define AVDTP_ERR_SEP_IN_USE               0x13
#define AVDTP_ERR_SEP_NOT_IN_USE           0x14
#define AVDTP_ERR_BAD_SERV_CATEGORY        0x17
#define AVDTP_ERR_BAD_PAYLOAD_FORMAT       0x18
#define AVDTP_ERR_NOT_SUPPORTED_COMMAND    0x19
#define AVDTP_ERR_INVALID_CAPABILITIES     0x1A
#define AVDTP_ERR_BAD_RECOVERY_TYPE        0x22
#define AVDTP_ERR_BAD_MEDIA_TRANSPORT_FORMAT 0x23
#define AVDTP_ERR_BAD_RECOVERY_FORMAT      0x25
#define AVDTP_ERR_BAD_ROHC_FORMAT          0x26
#define AVDTP_ERR_BAD_CP_FORMAT            0x27
#define AVDTP_ERR_BAD_MULTIPLEXING_FORMAT  0x28
#define AVDTP_ERR_UNSUPPORTED_CONFIGURATION 0x29
#define AVDTP_ERR_BAD_STATE                0x31
/* A2DP codec specific error codes (A2DP 5.1.3) */
#define A2DP_ERR_INVALID_CODEC_TYPE        0xC1
#define A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE  0xC2
#define A2DP_ERR_INVALID_SAMPLING_FREQUENCY 0xC3
#define A2DP_ERR_NOT_SUPPORTED_SAMPLING_FREQUENCY 0xC4
#define A2DP_ERR_INVALID_CHANNEL_MODE      0xC5
#define A2DP_ERR_NOT_SUPPORTED_CHANNEL_MODE 0xC6
#define A2DP_ERR_INVALID_SUBBANDS          0xC7
#define A2DP_ERR_NOT_SUPPORTED_SUBBANDS    0xC8
#define A2DP_ERR_INVALID_ALLOCATION_METHOD 0xC9
#define A2DP_ERR_NOT_SUPPORTED_ALLOCATION_METHOD 0xCA
#define A2DP_ERR_INVALID_MIN_BITPOOL       0xCB
#define A2DP_ERR_NOT_SUPPORTED_MIN_BITPOOL 0xCC
#define A2DP_ERR_INVALID_MAX_BITPOOL       0xCD
#define A2DP_ERR_NOT_SUPPORTED_MAX_BITPOOL 0xCE
#define A2DP_ERR_INVALID_BLOCK_LENGTH      0xD2
#define A2DP_ERR_INVALID_CP_TYPE           0xE0
#define A2DP_ERR_INVALID_CP_FORMAT         0xE1

/* Media types */
#define AVDTP_MEDIA_TYPE_AUDIO        0x00
/* TSEP */
#define AVDTP_TSEP_SOURCE             0x00
#define AVDTP_TSEP_SINK               0x01

#define AVDTP_MAX_SIGNAL_PAYLOAD      512    /* reassembled command payload */
#define AVDTP_MAX_SEPS                PNA2DP_MAX_REMOTE_SEPS
#define AVDTP_RESPONSE_TIMEOUT_MS     4000   /* TGAVDP100 is 100 ms per the spec, but sinks are slow */
#define AVDTP_LOCAL_SEP_COUNT         8

typedef struct _AVDTP_LOCAL_SEP {
    UCHAR   Seid;        /* 1..62 */
    UCHAR   InUse;
    UCHAR   MediaType;
    UCHAR   Tsep;
    ULONG   Codec;       /* PNA2DP_CODEC */
    UCHAR   CodecCaps[32];   /* capability IEs we advertise */
    UCHAR   CodecCapsLength;
    BOOLEAN DelayReporting;
} AVDTP_LOCAL_SEP, *PAVDTP_LOCAL_SEP;

typedef struct _AVDTP_REMOTE_SEP {
    UCHAR   Seid;
    UCHAR   InUse;
    UCHAR   MediaType;
    UCHAR   Tsep;
    BOOLEAN CapsValid;
    ULONG   ServiceCaps;     /* PNA2DP_SVC_* */
    ULONG   Codec;           /* PNA2DP_CODEC */
    UCHAR   MediaCodecType;  /* 0x00 SBC, 0x02 AAC, 0xFF vendor */
    ULONG   VendorId;
    USHORT  VendorCodecId;
    UCHAR   CodecCaps[32];
    UCHAR   CodecCapsLength;
} AVDTP_REMOTE_SEP, *PAVDTP_REMOTE_SEP;

typedef enum _AVDTP_STREAM_STATE {
    AvdtpStateIdle = 0,
    AvdtpStateConfigured,
    AvdtpStateOpening,
    AvdtpStateOpen,
    AvdtpStateStreaming,
    AvdtpStateClosing,
    AvdtpStateAborting
} AVDTP_STREAM_STATE;

/* A pending INT transaction. One at a time (AVDTP allows one outstanding command). */
typedef struct _AVDTP_TRANSACTION {
    UCHAR    Label;
    UCHAR    SignalId;
    BOOLEAN  Active;
    UCHAR    MessageType;       /* response type */
    UCHAR    ErrorCode;
    UCHAR    Payload[AVDTP_MAX_SIGNAL_PAYLOAD];
    ULONG    PayloadLength;
    KEVENT   Done;
} AVDTP_TRANSACTION;

/* Reassembly of fragmented signalling messages (START/CONTINUE/END) */
typedef struct _AVDTP_REASSEMBLY {
    BOOLEAN  Active;
    UCHAR    Label;
    UCHAR    MessageType;
    UCHAR    SignalId;
    UCHAR    Nosp;              /* remaining packets expected */
    UCHAR    Buffer[AVDTP_MAX_SIGNAL_PAYLOAD];
    ULONG    Length;
} AVDTP_REASSEMBLY;

struct _AVDTP_SESSION;
typedef struct _AVDTP_SESSION *PAVDTP_SESSION;

/* Callbacks into the device/stream layer. Called at PASSIVE_LEVEL from the AVDTP
 * work item, except OnMediaPacket (DISPATCH, sink role). */
typedef struct _AVDTP_CALLBACKS {
    PVOID Context;
    /* ACP: remote proposes a configuration for our local SEP. Return AVDTP error (0 = accept). */
    UCHAR (*OnRemoteSetConfiguration)(_In_ PVOID Ctx, _In_ PAVDTP_LOCAL_SEP Local, _In_ UCHAR IntSeid,
                                      _In_reads_bytes_(Len) const UCHAR *CodecIes, _In_ ULONG Len,
                                      _In_ ULONG ServiceCaps, _Out_ PUCHAR FailedCategory);
    UCHAR (*OnRemoteReconfigure)(_In_ PVOID Ctx, _In_reads_bytes_(Len) const UCHAR *CodecIes, _In_ ULONG Len,
                                 _Out_ PUCHAR FailedCategory);
    VOID  (*OnStreamStateChanged)(_In_ PVOID Ctx, _In_ AVDTP_STREAM_STATE Old, _In_ AVDTP_STREAM_STATE New,
                                  _In_ BOOLEAN RemoteInitiated);
    VOID  (*OnDelayReport)(_In_ PVOID Ctx, _In_ ULONG DelayTenthsMs);
    VOID  (*OnSignallingDown)(_In_ PVOID Ctx, _In_ NTSTATUS Status);
    VOID  (*OnMediaPacket)(_In_ PVOID Ctx, _In_reads_bytes_(Len) const UCHAR *Data, _In_ ULONG Len);
    VOID  (*OnMediaDown)(_In_ PVOID Ctx, _In_ NTSTATUS Status);
} AVDTP_CALLBACKS, *PAVDTP_CALLBACKS;

typedef struct _AVDTP_SESSION {
    PPN_BTH_LINK        Link;
    AVDTP_CALLBACKS     Cb;
    FAST_MUTEX          Mutex;            /* serializes INT procedures */
    KSPIN_LOCK          Lock;             /* protects transaction / state words */

    PN_L2CAP_CHANNEL    Signalling;
    PN_L2CAP_CHANNEL    Media;
    BOOLEAN             SignallingUp;
    BOOLEAN             MediaUp;
    BOOLEAN             MediaExpected;    /* we are waiting for the remote to open media */
    BOOLEAN             RemoteSignallingInitiated;

    USHORT              RemoteVersion;    /* 0x0100..0x0103 */
    AVDTP_LOCAL_SEP     LocalSeps[AVDTP_LOCAL_SEP_COUNT];
    ULONG               LocalSepCount;
    AVDTP_REMOTE_SEP    RemoteSeps[AVDTP_MAX_SEPS];
    ULONG               RemoteSepCount;
    BOOLEAN             RemoteSepsValid;

    /* The one stream we manage (A2DP uses a single stream per sink). */
    AVDTP_STREAM_STATE  StreamState;
    UCHAR               LocalSeid;        /* configured local SEP */
    UCHAR               RemoteSeid;       /* configured remote SEP */
    UCHAR               ConfiguredIes[32];
    UCHAR               ConfiguredIesLength;
    ULONG               ConfiguredServiceCaps;
    BOOLEAN             DelayReportingNegotiated;
    ULONG               SinkDelayTenthsMs;

    UCHAR               NextLabel;
    AVDTP_TRANSACTION   Txn;
    AVDTP_REASSEMBLY    Reasm;

    /* Deferred ACP command processing */
    PIO_WORKITEM        AcpWorkItem;
    LONG                AcpWorkQueued;
    KSPIN_LOCK          AcpQueueLock;
    LIST_ENTRY          AcpQueue;         /* AVDTP_ACP_ITEM */
    PDEVICE_OBJECT      DeviceObject;

    /* Deferred signalling-down processing */
    PIO_WORKITEM        DownWorkItem;
    LONG                DownQueued;
    NTSTATUS            DownStatus;
} AVDTP_SESSION;

typedef struct _AVDTP_ACP_ITEM {
    LIST_ENTRY  Link;
    UCHAR       Label;
    UCHAR       SignalId;
    ULONG       Length;
    UCHAR       Payload[AVDTP_MAX_SIGNAL_PAYLOAD];
} AVDTP_ACP_ITEM, *PAVDTP_ACP_ITEM;

/* Lifecycle */
NTSTATUS AvdtpInitialize(_Out_ PAVDTP_SESSION S, _In_ PPN_BTH_LINK Link, _In_ PDEVICE_OBJECT DeviceObject,
                         _In_ const AVDTP_CALLBACKS *Cb);
VOID     AvdtpUninitialize(_Inout_ PAVDTP_SESSION S);
VOID     AvdtpSetLocalSeps(_Inout_ PAVDTP_SESSION S, _In_reads_(Count) const AVDTP_LOCAL_SEP *Seps, _In_ ULONG Count);

/* Signalling channel management */
NTSTATUS AvdtpConnectSignalling(_Inout_ PAVDTP_SESSION S);          /* PASSIVE, INT */
VOID     AvdtpDisconnect(_Inout_ PAVDTP_SESSION S);                 /* PASSIVE, closes everything */
NTSTATUS AvdtpAcceptIncoming(_Inout_ PAVDTP_SESSION S, _In_ PINDICATION_PARAMETERS Params); /* DISPATCH */

/* INT procedures (PASSIVE; serialized by S->Mutex) */
NTSTATUS AvdtpDiscover(_Inout_ PAVDTP_SESSION S);                   /* DISCOVER + caps for all SEPs */
NTSTATUS AvdtpSetConfiguration(_Inout_ PAVDTP_SESSION S, _In_ UCHAR LocalSeid, _In_ UCHAR RemoteSeid,
                               _In_reads_bytes_(Len) const UCHAR *CodecIes, _In_ ULONG Len,
                               _In_ BOOLEAN RequestDelayReporting, _In_ BOOLEAN RequestContentProtection);
NTSTATUS AvdtpReconfigure(_Inout_ PAVDTP_SESSION S, _In_reads_bytes_(Len) const UCHAR *CodecIes, _In_ ULONG Len);
NTSTATUS AvdtpOpen(_Inout_ PAVDTP_SESSION S);                       /* OPEN + media channel connect */
NTSTATUS AvdtpStart(_Inout_ PAVDTP_SESSION S);
NTSTATUS AvdtpSuspend(_Inout_ PAVDTP_SESSION S);
NTSTATUS AvdtpClose(_Inout_ PAVDTP_SESSION S);
NTSTATUS AvdtpAbort(_Inout_ PAVDTP_SESSION S);

/* Media */
PPN_L2CAP_CHANNEL AvdtpMediaChannel(_In_ PAVDTP_SESSION S);
AVDTP_STREAM_STATE AvdtpGetState(_In_ PAVDTP_SESSION S);
USHORT   AvdtpMediaMtu(_In_ PAVDTP_SESSION S);

/* Codec IE helpers shared with a2dp_codec.cpp */
ULONG    AvdtpBuildMediaCodecCap(_Out_writes_bytes_(Cap) PUCHAR Out, _In_ ULONG Cap, _In_ UCHAR MediaCodecType,
                                 _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len);
