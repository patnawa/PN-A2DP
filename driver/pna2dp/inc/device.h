/*
 * device.h - per A2DP device context: ties the Bluetooth link, AVDTP session, pump,
 * codec host, configuration and the audio endpoint together and owns the connection
 * state machine.
 *
 * Locks (T8). Order: StateMutex -> IntentMutex -> pump internals -> StatusLock / spin
 * locks, and StateMutex -> Avdtp.Mutex -> Avdtp.StreamMutex (avdtp.h). IntentMutex and
 * the AVDTP locks are never held together.
 *   StateMutex   worker lock: the connection state machine (Reconcile, the AVDTP INT
 *                procedures it drives, session resets). May be held across Bluetooth I/O:
 *                a signalling connect (10 s), transactions (AVDTP_RESPONSE_TIMEOUT_MS each),
 *                discovery, OPEN + media connect. Taken only by the device work items
 *                (ConnectWorker, AudioWorker, ScoWorker), the AVDTP event work item
 *                (OnSignallingDown / OnMediaDown), DeviceStop and DeviceTeardown. Never by
 *                an audio-engine thread (stream open / close / state), a PortCls property
 *                handler, IOCTL dispatch, a DPC or a power callback.
 *   IntentMutex  short: what the audio endpoint and the user want (AudioStreamOpen and the
 *                format, IntentGen, configuration writes) and the pump's input / start /
 *                stop. Held only across CPU work, registry writes and the pump thread's
 *                step-batch wait; never across Bluetooth I/O and never while calling an
 *                AVDTP procedure. Audio-engine and IOCTL threads take only this lock:
 *                they record intent, touch the pump and queue the worker, which does the
 *                Bluetooth work (SUSPEND on stream close, CLOSE on renegotiation, the reset
 *                of DeviceReconnect).
 *   AVDTP callbacks run with Avdtp.StreamMutex held and take neither device mutex.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "transport_policy.h"   /* PN_REJECTED_SET, PnSetupRetry */

typedef enum _PN_AUDIO_REQUEST {
    PnAudioStop = 0,
    PnAudioPause,
    PnAudioRun
} PN_AUDIO_REQUEST;

typedef struct _PN_EVENT_WAITER {
    IO_CSQ      Csq;
    KSPIN_LOCK  Lock;
    LIST_ENTRY  Queue;
} PN_EVENT_WAITER;

typedef struct PNA2DP_DEVICE {
    ULONG               Signature;        /* 'PnA2' */
    PDEVICE_OBJECT      Fdo;
    PDEVICE_OBJECT      Pdo;
    PDRIVER_OBJECT      DriverObject;
    WDFDEVICE           WdfDevice;
    CPnAdapterCommon   *Adapter;          /* PortCls adapter object (owns us) */

    PN_BTH_LINK         Link;
    AVDTP_SESSION       Avdtp;
    PN_PUMP             Pump;
    PN_CODEC_HOST       Host;

    FAST_MUTEX          StateMutex;       /* worker lock: connect/configure/start/stop (see above) */
    FAST_MUTEX          IntentMutex;      /* short: audio intent, config writes, pump start/stop */
    KSPIN_LOCK          StatusLock;
    PNA2DP_CONFIG       Config;
    PNA2DP_CONFIG       WorkConfig;       /* the worker's copy of Config for one pass (StateMutex) */
    LONG                SinkDelaySetting; /* interlocked: Config.SinkDelayMs / SinkDelayCodec as one word
                                           * (PnSinkDelaySettingPack) for the lock-free DeviceSinkDelayUs;
                                           * written with Config under StatusLock */
    PNA2DP_CAPABILITIES Caps;
    PNA2DP_STATUS       Status;
    A2DP_CODEC_CONFIG   Active;           /* negotiated codec configuration */
    BOOLEAN             ActiveValid;

    /* audio endpoint state */
    PN_AUDIO_REQUEST    AudioRequest;     /* what the audio engine wants */
    ULONG               AudioRate;
    ULONG               AudioChannels;
    ULONG               AudioBits;
    ULONG               AudioValidBits;
    BOOLEAN             AudioFloat;
    BOOLEAN             AudioStreamOpen;
    ULONG               IntentGen;        /* +1 per stream open/close, renegotiation, reset (IntentMutex) */
    LONG                RenegotiatePending; /* interlocked: DeviceSetConfig asks the worker to CLOSE */
    LONG                UserResetPending;   /* interlocked: DeviceReconnect asks the worker to reset */
    LONG                UserDisconnectPending; /* interlocked: DeviceRequestDisconnect asks the worker to drop the session */
    LONG                UserDisconnected;   /* interlocked: no automatic connect until a reconnect, a new
                                             * local playback start or an accepted incoming signalling channel (C7) */
    LONG                UserConnectPending; /* interlocked: DeviceRequestConnect asks the worker to start the
                                             * back-off over (review #12) */
    LONG                VolumeDb16;
    BOOLEAN             Muted;
    BOOLEAN             RemoteVolumeActive; /* Windows AVRCP service confirmed receiver volume */
    BOOLEAN             ReceiverHoldsVolume; /* absolute volume ended; the receiver keeps ReceiverHeldDb16 (#2/#27) */
    LONG                ReceiverHeldDb16;
    volatile LONGLONG   RenderHoldUntilHns; /* a stream start is pending: the WaveRT clock holds until then (#24), 0 = none */

    /* work items */
    PIO_WORKITEM        ConnectWorkItem;
    LONG                ConnectQueued;
    PIO_WORKITEM        AudioWorkItem;
    LONG                AudioQueued;
    PIO_WORKITEM        ScoWorkItem;
    LONG                ScoQueued;
    KTIMER              SuspendTimer;      /* delayed SUSPEND after pause */
    KDPC                SuspendDpc;
    KTIMER              ReconnectTimer;
    KDPC                ReconnectDpc;
    ULONG               ReconnectAttempts;
    LONG                ResetReconnectBudget;
    LONG                PowerSuspended;
    LONG                PowerResumePending;
    LONG                PowerIdle;         /* Dx while the system stays in S0: SUSPEND, keep signalling */
    LONG                Removing;

    /* transport recovery (transport_policy.h); worker state under StateMutex unless noted */
    PnSetupRetry        SetupRetry;        /* setup-failure back-off, reset by continuous streaming */
    ULONG               ConsecutiveTimeouts;
    ULONG               SessionGen;        /* signalling generation Rejected / CodecUpgradeTried belong to */
    PN_REJECTED_SET     Rejected;          /* (codec, remote SEID) pairs the receiver refused */
    BOOLEAN             CodecUpgradeTried;
    BOOLEAN             RemoteConfigured;  /* the receiver configured the current stream (AVDTP callback) */
    BOOLEAN             ActiveStreamed;    /* the pump started on the current stream configuration: only such a
                                            * stream is bridged across a Windows rate change (PnStreamBridged);
                                            * set by StartPumpIfNeeded, cleared with each new configuration */
    BOOLEAN             SdpVersionApplied;
    BOOLEAN             ReceiverUnreachable;
    ULONG               OpenCollisions;    /* consecutive OPENs that crossed the receiver's (review #3) */
    LONGLONG            RemoteConfiguredHns;
    LONGLONG            RemoteSuspendHns;
    LONG                RemoteSuspended;      /* interlocked: the receiver suspended our stream */
    LONG                RemoteStartedPending; /* interlocked: the receiver started it */
    LONG                LocalStartRequested;  /* interlocked: playback (re)started since */
    LONG                RemoteSuspendRetried; /* interlocked: our one unprompted START of this suspend episode went out (T2) */
    LONGLONG            RemoteSuspendRetryHns;
    PN_RESUME_DETECT    ResumeDetect;         /* wave Tick only: audio resumed in a running endpoint (T2) */
    LONG                MediaStallPending;    /* interlocked: C4 */
    LONG                LinkLostPending;      /* interlocked: ACL down for our address */
    LONG                SdpBusy;              /* interlocked: an SDP query is running (QuerySdpOnce) */
    BOOLEAN             SdpSucceeded;         /* written by the SdpBusy owner */
    ULONG               SdpFailures;
    ULONG               SdpFailedSession;     /* signalling generation of the last failure (review #5) */
    ULONG               FormatsKey;           /* negotiated codec/rate last reported to the endpoint (C6, StateMutex) */
    BOOLEAN             Stopped;           /* DeviceStop done */
    BOOLEAN             TornDown;          /* DeviceTeardown done */

    /* Lifetime: one reference for the adapter plus one per open control handle. The
     * memory outlives DeviceStop/DeviceTeardown so audio streams and control handles that
     * are still open after a surprise removal only ever see a quiesced device. */
    LONG                RefCount;
    EX_RUNDOWN_REF      WorkRundown;       /* queued/running device work items */
    EX_RUNDOWN_REF      ControlRundown;    /* control IOCTLs in progress */

    /* event waiters (IOCTL_PNA2DP_WAIT_EVENT) */
    PN_EVENT_WAITER     Waiters;
    ULONG               PendingEvents;

    /* control interface */
    UNICODE_STRING      InterfaceSymlink;
    BOOLEAN             InterfaceRegistered;
    LONG                OpenHandles;

    LIST_ENTRY          GlobalLink;
} PNA2DP_DEVICE;

#define PNA2DP_DEVICE_SIGNATURE  0x32416E50u  /* 'PnA2' */

/* Lifecycle (PASSIVE)
 *   DeviceCreate     allocate, refcount 1 (the adapter's reference)
 *   DeviceStart      bring up link/pump/AVDTP; on failure the caller still runs Stop/Teardown
 *   DeviceStop       quiesce: no more work, timers, IOCTLs or Bluetooth callbacks (idempotent)
 *   DeviceTeardown   free the subsystems; the structure stays valid (idempotent)
 *   DeviceRelease    drop a reference; the last one frees the memory */
VOID     DeviceListInitialize(void);                     /* DriverEntry */
NTSTATUS DeviceCreate(_In_ PDEVICE_OBJECT Fdo, _In_ PDEVICE_OBJECT Pdo, _In_ PDRIVER_OBJECT DriverObject,
                      _In_ CPnAdapterCommon *Adapter, _Out_ PPNA2DP_DEVICE *Device);
NTSTATUS DeviceStart(_Inout_ PPNA2DP_DEVICE D);
VOID     DeviceStop(_Inout_ PPNA2DP_DEVICE D);
VOID     DeviceTeardown(_Inout_ PPNA2DP_DEVICE D);
VOID     DeviceReference(_Inout_ PPNA2DP_DEVICE D);
VOID     DeviceRelease(_In_ PPNA2DP_DEVICE D);
PPNA2DP_DEVICE DeviceReferenceFromFdo(_In_ PDEVICE_OBJECT Fdo);   /* referenced; NULL if not started */

/* Audio endpoint -> device. None of these waits for Bluetooth I/O (T8). */
NTSTATUS DeviceAudioStreamOpened(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG Rate, _In_ ULONG Channels,
                                 _In_ ULONG ContainerBits, _In_ ULONG ValidBits, _In_ BOOLEAN IsFloat);
VOID     DeviceAudioStreamClosed(_Inout_ PPNA2DP_DEVICE D);
VOID     DeviceAudioStateChanged(_Inout_ PPNA2DP_DEVICE D, _In_ PN_AUDIO_REQUEST Request);   /* DISPATCH ok */
/* T2 (review round 4 #7), from the wave Tick (DISPATCH ok): DeviceAwaitsAudioResume says
 * whether the PCM is worth checking for a signal; DeviceNoteRenderedAudio reports it. */
BOOLEAN  DeviceAwaitsAudioResume(_In_ PPNA2DP_DEVICE D);
/* wave Tick (DISPATCH ok): hold the render clock, a stream start is pending (wave_math.h) */
BOOLEAN  DeviceRenderHold(_In_ PPNA2DP_DEVICE D, _In_ LONGLONG NowHns);
VOID     DeviceNoteRenderedAudio(_Inout_ PPNA2DP_DEVICE D, _In_ BOOLEAN Watching, _In_ BOOLEAN Audible, _In_ ULONG ElapsedUs);
VOID     DeviceAudioVolumeChanged(_Inout_ PPNA2DP_DEVICE D, _In_ LONG VolumeDb16, _In_ BOOLEAN Muted);

/* Control interface -> device */
NTSTATUS DeviceGetStatus(_In_ PPNA2DP_DEVICE D, _Out_ PPNA2DP_STATUS Status);
NTSTATUS DeviceGetCapabilities(_In_ PPNA2DP_DEVICE D, _Out_ PPNA2DP_CAPABILITIES Caps);
NTSTATUS DeviceGetConfig(_In_ PPNA2DP_DEVICE D, _Out_ PPNA2DP_CONFIG Cfg);
NTSTATUS DeviceSetConfig(_Inout_ PPNA2DP_DEVICE D, _In_ const PNA2DP_CONFIG *Cfg);   /* PASSIVE, no Bluetooth wait */
NTSTATUS DeviceReconnect(_Inout_ PPNA2DP_DEVICE D);                                    /* PASSIVE, no Bluetooth wait */
NTSTATUS DeviceRequestDisconnect(_Inout_ PPNA2DP_DEVICE D);                            /* PASSIVE, no Bluetooth wait (C7) */
NTSTATUS DeviceRequestConnect(_Inout_ PPNA2DP_DEVICE D);                               /* PASSIVE, no Bluetooth wait (ONESHOT_RECONNECT) */
VOID     DeviceNotifyEvent(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG EventMask);           /* any IRQL <= DISPATCH */
NTSTATUS DeviceQueueWaitEvent(_Inout_ PPNA2DP_DEVICE D, _In_ PIRP Irp);
VOID     DeviceFlushWaitersForFile(_Inout_ PPNA2DP_DEVICE D, _In_ PFILE_OBJECT File);
VOID     DeviceHostChanged(_Inout_ PPNA2DP_DEVICE D);                                 /* codec host (un)registered */
VOID     DevicePowerChanged(_Inout_ PPNA2DP_DEVICE D, _In_ DEVICE_POWER_STATE State, _In_ SYSTEM_POWER_STATE SystemState);
ULONG    DeviceSinkDelayUs(_In_ PPNA2DP_DEVICE D);   /* <= DISPATCH, lock-free: receiver latency of the stream (as applied) */
VOID     DeviceMediaStalled(_Inout_ PPNA2DP_DEVICE D);  /* <= DISPATCH: media writes stopped completing (C4) */

/* Scheduling */
VOID     DeviceQueueConnect(_Inout_ PPNA2DP_DEVICE D);
