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
 * Locks (T9). Order: device StateMutex -> Mutex -> StreamMutex -> Channel.StartMutex ->
 * spin locks (Lock, ReasmLock, AcpQueueLock, channel locks).
 *   Mutex        command lock: one INT procedure (our outgoing command and what follows
 *                from it) at a time. Held across the wait for the receiver's answer (up to
 *                AVDTP_RESPONSE_TIMEOUT_MS per command) and our media channel connect (up to
 *                10 s). Taken only by the INT procedures, AvdtpDisconnect and
 *                AvdtpResetStream, i.e. by the device worker under its StateMutex and by the
 *                event worker for a new signalling session (after waking the old session's
 *                transaction and cancelling its media connect). Never by the acceptor.
 *   StreamMutex  short stream lock: StreamState transitions and the stream's words
 *                (Local/RemoteSeid, ConfiguredIes, MediaUp/MediaExpected/OpenSent/IntSignal,
 *                RemoteSeps, sink delay). Never held across a wait for the receiver or for a
 *                channel connect, disconnect or reader start. Taken by the acceptor (one
 *                command at a time), the event worker's stream decisions and the INT
 *                procedures before and after their transaction (AvdtpTransact returns
 *                holding it; AvdtpTxnEnd releases it once the answer is applied).
 * So the acceptor answers within milliseconds while a command of ours is outstanding; it
 * judges the receiver's commands against IntSignal, and our procedures judge the answer
 * against the state as it is then (PnIntOutcome). A receiver command that arrived behind
 * the answer to ours waits (bounded) until that answer is applied (PnAcpMustWait); one
 * that arrived before the answer is finished before the answer is judged (bounded,
 * AvdtpTransact without StreamMutex: PnIntMustWaitForAcp). The
 * AVDTP callbacks below run with StreamMutex held: they must not take the device
 * StateMutex / IntentMutex or any AVDTP lock, and must not wait.
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
    ULONG    AcpBefore;         /* AcpQueuedSeq when the answer arrived */
    KEVENT   Done;
} AVDTP_TRANSACTION;

struct _AVDTP_SESSION;
typedef struct _AVDTP_SESSION *PAVDTP_SESSION;

/* Callbacks into the device/stream layer. Called at PASSIVE_LEVEL from the AVDTP
 * work items, except OnMediaPacket (DISPATCH, sink role). The stream callbacks run with
 * StreamMutex held (see above); OnSignallingDown / OnMediaDown run with no AVDTP lock. */
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
    /* ACP: the receiver STARTed (TRUE) or SUSPENDed (FALSE) the stream; called before the
     * state change, so the worker that change queues sees it (T2) */
    VOID  (*OnRemoteStreamControl)(_In_ PVOID Ctx, _In_ BOOLEAN Started);
} AVDTP_CALLBACKS, *PAVDTP_CALLBACKS;

typedef struct _AVDTP_SESSION {
    PPN_BTH_LINK        Link;
    AVDTP_CALLBACKS     Cb;
    FAST_MUTEX          Mutex;            /* command lock: serializes INT procedures (see above) */
    FAST_MUTEX          StreamMutex;      /* short stream lock, never held across a wait */
    KSPIN_LOCK          Lock;             /* protects transaction / state words */

    PN_L2CAP_CHANNEL    Signalling;
    PN_L2CAP_CHANNEL    Media;
    BOOLEAN             SignallingUp;
    BOOLEAN             MediaUp;
    BOOLEAN             MediaExpected;    /* we are waiting for the remote to open media */
    BOOLEAN             RemoteSignallingInitiated;
    LONG                ResetPending;     /* interlocked: a new signalling session was accepted while a stream of
                                           * the old one existed; no command goes out until AvdtpResetStream ran
                                           * (review #1) */

    USHORT              RemoteVersion;    /* 0x0100..0x0103 */
    AVDTP_LOCAL_SEP     LocalSeps[AVDTP_LOCAL_SEP_COUNT];
    ULONG               LocalSepCount;
    AVDTP_REMOTE_SEP    RemoteSeps[AVDTP_MAX_SEPS];
    ULONG               RemoteSepCount;
    BOOLEAN             RemoteSepsValid;
    BOOLEAN             CapsBasicOnly;    /* GET_ALL_CAPABILITIES failed once: GET_CAPABILITIES for this device (T5) */
    BOOLEAN             OpenSent;         /* our OPEN is outstanding: an incoming channel is its transport (T9) */
    BOOLEAN             OpenCollided;     /* the acceptor refused a receiver OPEN during ours (review #3) */
    UCHAR               IntSignal;        /* our outstanding command, for the acceptor (T9) */
    ULONG               MediaHeldGen;     /* receiver's media channel up before our OPEN's answer: adopt it */

    /* The one stream we manage (A2DP uses a single stream per sink). */
    AVDTP_STREAM_STATE  StreamState;
    UCHAR               LocalSeid;        /* configured local SEP */
    UCHAR               RemoteSeid;       /* configured remote SEP */
    UCHAR               ConfiguredIes[32];
    UCHAR               ConfiguredIesLength;
    ULONG               ConfiguredServiceCaps;
    BOOLEAN             DelayReportingNegotiated;
    ULONG               SinkDelayTenthsMs;   /* last DELAYREPORT of this stream; 0 at each configuration and Idle */

    UCHAR               NextLabel;
    AVDTP_TRANSACTION   Txn;
    ULONG               ResponseSeq;      /* +1 per answer matched to our command (receive path) */
    ULONG               AppliedSeq;       /* answers our INT procedure has applied (AvdtpTxnEnd) */
    KEVENT              TxnApplied;       /* set when AppliedSeq advances, cleared by each answer */
    KEVENT              TransportIdle;    /* clear while our OPEN connects / our CLOSE, ABORT drops the media channel */
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
    ULONG               AcpQueuedSeq;     /* +1 per command queued (receive path, ReasmLock) */
    volatile LONG       AcpDoneSeq;       /* +1 per queued command the acceptor finished */
    KEVENT              AcpProgress;      /* set when AcpDoneSeq advances */
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
    ULONG       AfterResponse;    /* ResponseSeq when it arrived: that answer is applied first */
    UCHAR       Payload[AVDTP_MAX_SIGNAL_PAYLOAD];
} AVDTP_ACP_ITEM, *PAVDTP_ACP_ITEM;

/* Lifecycle */
NTSTATUS AvdtpInitialize(_Out_ PAVDTP_SESSION S, _In_ PPN_BTH_LINK Link, _In_ PDEVICE_OBJECT DeviceObject,
                         _In_ const AVDTP_CALLBACKS *Cb);
VOID     AvdtpUninitialize(_Inout_ PAVDTP_SESSION S);
VOID     AvdtpStopWork(_Inout_ PAVDTP_SESSION S);    /* PASSIVE: refuse new work, wait for queued work */
VOID     AvdtpSetLocalSeps(_Inout_ PAVDTP_SESSION S, _In_reads_(Count) const AVDTP_LOCAL_SEP *Seps, _In_ ULONG Count);
/* <= DISPATCH: advertise the local SEP of Codec in use (unavailable) or free in DISCOVER (T10) */
VOID     AvdtpSetLocalSepAvailable(_Inout_ PAVDTP_SESSION S, _In_ ULONG Codec, _In_ BOOLEAN Available);

/* Signalling channel management */
NTSTATUS AvdtpConnectSignalling(_Inout_ PAVDTP_SESSION S);          /* PASSIVE, INT */
VOID     AvdtpDisconnect(_Inout_ PAVDTP_SESSION S);                 /* PASSIVE, closes everything */
/* DISPATCH. Fails for a connection it refused; *Signalling: the channel is our signalling channel. */
NTSTATUS AvdtpAcceptIncoming(_Inout_ PAVDTP_SESSION S, _In_ PINDICATION_PARAMETERS Params, _Out_ PBOOLEAN Signalling);

/* INT procedures (PASSIVE; serialized by S->Mutex; they wait for Bluetooth I/O, so only
 * the device worker calls them, never an audio-engine, property or IOCTL thread: T8) */
NTSTATUS AvdtpDiscover(_Inout_ PAVDTP_SESSION S);                   /* DISCOVER + caps for all SEPs */
NTSTATUS AvdtpSetConfiguration(_Inout_ PAVDTP_SESSION S, _In_ UCHAR LocalSeid, _In_ UCHAR RemoteSeid,
                               _In_reads_bytes_(Len) const UCHAR *CodecIes, _In_ ULONG Len,
                               _In_ BOOLEAN RequestDelayReporting, _In_ BOOLEAN RequestContentProtection,
                               _Out_opt_ PUCHAR ErrorCode);      /* AVDTP/A2DP error of a reject */
NTSTATUS AvdtpReconfigure(_Inout_ PAVDTP_SESSION S, _In_reads_bytes_(Len) const UCHAR *CodecIes, _In_ ULONG Len);
/* OPEN + media channel connect; PENDING: the receiver's channel is adopted. ReceiverOpenRefused:
 * the acceptor refused the receiver's own OPEN while ours was outstanding (a collision). */
NTSTATUS AvdtpOpen(_Inout_ PAVDTP_SESSION S, _Out_opt_ PUCHAR ErrorCode, _Out_opt_ PBOOLEAN ReceiverOpenRefused);
NTSTATUS AvdtpStart(_Inout_ PAVDTP_SESSION S);
NTSTATUS AvdtpSuspend(_Inout_ PAVDTP_SESSION S);
NTSTATUS AvdtpClose(_Inout_ PAVDTP_SESSION S);
NTSTATUS AvdtpAbort(_Inout_ PAVDTP_SESSION S);
NTSTATUS AvdtpProbe(_Inout_ PAVDTP_SESSION S);   /* GET_CAPABILITIES of the stream's SEP: does the receiver answer? */

/* Media */
PPN_L2CAP_CHANNEL AvdtpMediaChannel(_In_ PAVDTP_SESSION S);
AVDTP_STREAM_STATE AvdtpGetState(_In_ PAVDTP_SESSION S);
USHORT   AvdtpMediaMtu(_In_ PAVDTP_SESSION S);
