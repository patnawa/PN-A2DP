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

#include "avdtp_wire.h"

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
    VOID  (*OnSignallingDown)(_In_ PVOID Ctx, _In_ NTSTATUS Status, _In_ ULONG Generation);   /* recheck under StateMutex */
    VOID  (*OnMediaPacket)(_In_ PVOID Ctx, _In_reads_bytes_(Len) const UCHAR *Data, _In_ ULONG Len);
    VOID  (*OnMediaDown)(_In_ PVOID Ctx, _In_ NTSTATUS Status, _In_ ULONG Generation);
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
    KSPIN_LOCK          ReasmLock;        /* signalling readers complete concurrently */

    /* Work items hold WorkRundown from queueing to the end of the worker. */
    EX_RUNDOWN_REF      WorkRundown;
    BOOLEAN             WorkReady;
    BOOLEAN             WorkStopped;

    /* Deferred ACP command processing */
    PIO_WORKITEM        AcpWorkItem;
    LONG                AcpWorkQueued;
    ULONG               AcpQueueDepth;
    KSPIN_LOCK          AcpQueueLock;
    LIST_ENTRY          AcpQueue;         /* AVDTP_ACP_ITEM */
    PDEVICE_OBJECT      DeviceObject;

    /* Deferred signalling-down processing */
    PIO_WORKITEM        DownWorkItem;
    LONG                DownQueued;
    NTSTATUS            DownStatus;
    /* channel generations the queued events refer to (PN_L2CAP_CHANNEL::Generation) */
    ULONG               SigDownGen;
    ULONG               MediaDownGen;
    ULONG               SigAcceptGen;
    ULONG               MediaAcceptGen;
    ULONG               SigUpDoneGen;     /* accept generations the event worker has handled */
    ULONG               MediaUpDoneGen;
} AVDTP_SESSION;

typedef struct _AVDTP_ACP_ITEM {
    LIST_ENTRY  Link;
    UCHAR       Label;
    UCHAR       SignalId;
    ULONG       Length;
    ULONG       Generation;
    UCHAR       Payload[AVDTP_MAX_SIGNAL_PAYLOAD];
} AVDTP_ACP_ITEM, *PAVDTP_ACP_ITEM;

/* Lifecycle */
NTSTATUS AvdtpInitialize(_Out_ PAVDTP_SESSION S, _In_ PPN_BTH_LINK Link, _In_ PDEVICE_OBJECT DeviceObject,
                         _In_ const AVDTP_CALLBACKS *Cb);
VOID     AvdtpUninitialize(_Inout_ PAVDTP_SESSION S);
VOID     AvdtpStopWork(_Inout_ PAVDTP_SESSION S);    /* PASSIVE: refuse new work, wait for queued work */
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
