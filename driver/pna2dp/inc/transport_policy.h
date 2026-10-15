/* Pure transport decisions shared with core_test: codec/SEP choice, the Reconcile plan,
 * setup-failure handling, discovery fallback, stream close, power, sink delay and link
 * events. Nothing here does I/O, allocates or blocks; device.cpp and avdtp.cpp snapshot
 * their state, call these and execute the result.
 *
 * Inputs marked "corrected transport" carry the state the transport review fixes (T1-T13)
 * decide on; core_test_T.inl covers every decision.
 *
 * SPDX-License-Identifier: MIT */
#pragma once
#include "codec_core.h"
#include "avdtp_wire.h"
#include "audio_policy.h"
#include "../../../common/recovery_policy.h"

/* ---- NTSTATUS values the decisions look at ------------------------------------- */
#define PN_ST_PENDING                ((LONG)0x00000103L)
#define PN_ST_DEVICE_NOT_READY       ((LONG)0xC00000A3L)
#define PN_ST_IO_TIMEOUT             ((LONG)0xC00000B5L)
#define PN_ST_NOT_SUPPORTED          ((LONG)0xC00000BBL)
#define PN_ST_CANCELLED              ((LONG)0xC0000120L)
#define PN_ST_INVALID_DEVICE_STATE   ((LONG)0xC0000184L)
#define PN_ST_DEVICE_PROTOCOL_ERROR  ((LONG)0xC0000186L)
#define PN_ST_CONNECTION_DISCONNECTED ((LONG)0xC000020CL)
#ifdef _KERNEL_MODE
static_assert(PN_ST_PENDING == STATUS_PENDING && PN_ST_IO_TIMEOUT == STATUS_IO_TIMEOUT &&
              PN_ST_NOT_SUPPORTED == STATUS_NOT_SUPPORTED && PN_ST_CANCELLED == STATUS_CANCELLED &&
              PN_ST_INVALID_DEVICE_STATE == STATUS_INVALID_DEVICE_STATE &&
              PN_ST_DEVICE_PROTOCOL_ERROR == STATUS_DEVICE_PROTOCOL_ERROR &&
              PN_ST_CONNECTION_DISCONNECTED == STATUS_CONNECTION_DISCONNECTED &&
              PN_ST_DEVICE_NOT_READY == STATUS_DEVICE_NOT_READY, "NTSTATUS mirror");
#endif
static inline BOOLEAN PnStOk(LONG Status) { return Status >= 0; }

/* ---- Encoder ownership (values match PN_ENCODER_OWNER) -------------------------- */
#define PN_OWNER_NONE        0
#define PN_OWNER_KERNEL_SBC  1
#define PN_OWNER_HOST        2

/* HostMask: PNA2DP_CODEC_BIT of every codec the codec host can encode right now. */
static inline BOOLEAN PnEncoderFor(ULONG Codec, ULONG ConfigFlags, ULONG HostMask, UCHAR *Owner)
{
    if (Codec == PNA2DP_CODEC_SBC && !(ConfigFlags & PNA2DP_CFG_HOST_CODECS_ONLY)) { *Owner = PN_OWNER_KERNEL_SBC; return TRUE; }
    if (Codec < 32 && (HostMask & PNA2DP_CODEC_BIT(Codec))) { *Owner = PN_OWNER_HOST; return TRUE; }
    if (Codec == PNA2DP_CODEC_SBC) { *Owner = PN_OWNER_KERNEL_SBC; return TRUE; }
    *Owner = PN_OWNER_NONE;
    return FALSE;
}

/* ---- (codec, remote SEID) pairs the receiver refused in this session ------------- */
#define PN_REJECTED_MAX 16
typedef struct _PN_REJECTED_SET {
    ULONG Count;
    UCHAR Codec[PN_REJECTED_MAX];
    UCHAR Seid[PN_REJECTED_MAX];
} PN_REJECTED_SET;

static inline BOOLEAN PnRejectedContains(const PN_REJECTED_SET *S, ULONG Codec, UCHAR Seid)
{
    if (!S) return FALSE;
    for (ULONG i = 0; i < S->Count && i < PN_REJECTED_MAX; i++) if (S->Codec[i] == Codec && S->Seid[i] == Seid) return TRUE;
    return FALSE;
}

static inline VOID PnRejectedAdd(PN_REJECTED_SET *S, ULONG Codec, UCHAR Seid)
{
    if (PnRejectedContains(S, Codec, Seid) || S->Count >= PN_REJECTED_MAX) return;
    S->Codec[S->Count] = (UCHAR)Codec;
    S->Seid[S->Count] = Seid;
    S->Count++;
}

/* ---- Codec / SEP choice ---------------------------------------------------------- */
typedef struct _PN_CODEC_CHOICE_INPUT {
    const AVDTP_REMOTE_SEP *RemoteSeps;
    ULONG                   RemoteSepCount;
    const AVDTP_LOCAL_SEP  *LocalSeps;
    ULONG                   LocalSepCount;
    const PNA2DP_CONFIG    *Config;
    ULONG                   HostCodecMask;   /* see PnEncoderFor */
    BOOLEAN                 RestrictRate;    /* an audio stream is open: carry its rate */
    ULONG                   PreferredRate;
    ULONG                   PreferredBits;
    BOOLEAN                 Conservative;    /* AUTO_QUALITY on an adapter without EDR */
    /* corrected transport */
    ULONG                   RateConvert;     /* PN_RATECONV_*: engine rates the pump bridges (C8, #1) */
    UCHAR                   OwnRemoteSeid;   /* remote SEP of our own current stream, 0 = none */
    const PN_REJECTED_SET  *Rejected;
} PN_CODEC_CHOICE_INPUT;

typedef struct _PN_CODEC_CHOICE {
    ULONG             RemoteIndex;
    UCHAR             RemoteSeid;
    UCHAR             LocalSeid;
    UCHAR             Owner;          /* PN_OWNER_* */
    ULONG             ServiceCaps;    /* of the remote SEP */
    A2DP_CODEC_CONFIG Cfg;
} PN_CODEC_CHOICE;

static inline const AVDTP_LOCAL_SEP *PnLocalSepForCodec(const AVDTP_LOCAL_SEP *Seps, ULONG Count, ULONG Codec)
{
    for (ULONG i = 0; i < Count; i++) if (Seps[i].Codec == Codec) return &Seps[i];
    return NULL;
}

/* Stream rate for an engine rate: the engine rate itself whenever the sink takes it (nothing to
 * convert); else what RateConvert bridges: exactly half of a 96 / 88.2 kHz engine rate (C8,
 * PN_RATECONV_HALF and up), then 44.1 or 48 kHz, the engine rate's own family first (44.1 for
 * 44.1 / 88.2 kHz, 48 for 48 / 96 kHz; PN_RATECONV_ANY, 0.1.0.22 #1); else EngineRate, which the
 * sink lacks: the caller's selection fails, as before. */
static inline ULONG PnStreamRateFor(ULONG SinkRateMask, ULONG EngineRate, ULONG RateConvert)
{
    if (SinkRateMask & A2dpHzToRateMask(EngineRate)) return EngineRate;
    BOOLEAN family44 = EngineRate % 11025 == 0;
    const ULONG candidates[3] = { EngineRate / 2, family44 ? 44100u : 48000u, family44 ? 48000u : 44100u };
    for (ULONG i = 0; i < 3; i++)
        if ((SinkRateMask & A2dpHzToRateMask(candidates[i])) && PnRateConvertible(EngineRate, candidates[i], RateConvert)) return candidates[i];
    return EngineRate;
}

/* T1: a receiver reports a SEP in use while any stream uses it, ours included. Our own
 * stream's SEP is free for us; another in-use SEP is tried only after every free one. */
static inline BOOLEAN PnSepFreeFor(const PN_CODEC_CHOICE_INPUT *In, const AVDTP_REMOTE_SEP *R)
{
    return !R->InUse || (In->OwnRemoteSeid != 0 && R->Seid == In->OwnRemoteSeid);
}

/* One remote SEP against one codec: TRUE fills Cfg. AllowInUse: the second pass. */
static inline BOOLEAN PnTrySep(const PN_CODEC_CHOICE_INPUT *In, const AVDTP_REMOTE_SEP *R, ULONG Codec, BOOLEAN AllowInUse,
                               A2DP_CODEC_CONFIG *Cfg)
{
    if (!R->CapsValid || R->Codec != Codec || R->Tsep != AVDTP_TSEP_SINK) return FALSE;
    if (!AllowInUse && !PnSepFreeFor(In, R)) return FALSE;
    if (PnRejectedContains(In->Rejected, Codec, R->Seid)) return FALSE;   /* T4: refused in this session */
    A2DP_CODEC_CAPS caps;
    if (!A2dpParseCaps(R->MediaCodecType, R->CodecCaps, R->CodecCapsLength, &caps, NULL, NULL)) return FALSE;
    ULONG rate = In->PreferredRate, bits = In->PreferredBits;
    if (In->RestrictRate) {
        rate = PnStreamRateFor(caps.RateMask, In->PreferredRate, In->RateConvert);
        caps.RateMask &= A2dpHzToRateMask(rate);
        /* converted audio carries more than 16 bits: LDAC's automatic encoder input takes S32 */
        if (rate != In->PreferredRate && bits < 24) bits = 24;
    }
    if (!A2dpSelectConfig(&caps, In->Config, rate, bits, Cfg)) return FALSE;
    if (In->Conservative && !A2dpConstrainBasicRadio(Cfg)) return FALSE;
    return TRUE;
}

/* Codec priority list first (enabled, encoder available, local SEP present), then SBC
 * with any sink SEP as the last resort; all of it with free SEPs, then once more with the
 * SEPs the receiver reported in use. */
static inline BOOLEAN PnChooseCodecConfig(const PN_CODEC_CHOICE_INPUT *In, PN_CODEC_CHOICE *Out)
{
    const PNA2DP_CONFIG *c = In->Config;
    for (ULONG pass = 0; pass < 2; pass++) {
        BOOLEAN allowInUse = pass != 0;
        for (ULONG pi = 0; pi < PN_ARRAYSIZE(c->CodecPriority); pi++) {
            ULONG codec = c->CodecPriority[pi];
            if (!codec) break;
            if (codec >= 32 || !(c->EnabledCodecMask & PNA2DP_CODEC_BIT(codec))) continue;
            UCHAR owner;
            if (!PnEncoderFor(codec, c->Flags, In->HostCodecMask, &owner)) continue;
            const AVDTP_LOCAL_SEP *local = PnLocalSepForCodec(In->LocalSeps, In->LocalSepCount, codec);
            if (!local) continue;
            for (ULONG i = 0; i < In->RemoteSepCount; i++) {
                const AVDTP_REMOTE_SEP *r = &In->RemoteSeps[i];
                if (!PnTrySep(In, r, codec, allowInUse, &Out->Cfg)) continue;
                Out->RemoteIndex = i; Out->RemoteSeid = r->Seid; Out->ServiceCaps = r->ServiceCaps;
                Out->Owner = owner; Out->LocalSeid = local->Seid;
                return TRUE;
            }
        }
        /* last resort: SBC with any sink SEP */
        for (ULONG i = 0; i < In->RemoteSepCount; i++) {
            const AVDTP_REMOTE_SEP *r = &In->RemoteSeps[i];
            if (!PnTrySep(In, r, PNA2DP_CODEC_SBC, allowInUse, &Out->Cfg)) continue;
            const AVDTP_LOCAL_SEP *local = PnLocalSepForCodec(In->LocalSeps, In->LocalSepCount, PNA2DP_CODEC_SBC);
            Out->RemoteIndex = i; Out->RemoteSeid = r->Seid; Out->ServiceCaps = r->ServiceCaps;
            Out->Owner = PN_OWNER_KERNEL_SBC; Out->LocalSeid = local ? local->Seid : 1;
            return TRUE;
        }
    }
    return FALSE;
}

/* Position of Codec in the priority list (smaller is better), 0xFFFFFFFF if absent. */
static inline ULONG PnCodecRank(const PNA2DP_CONFIG *Config, ULONG Codec)
{
    for (ULONG i = 0; i < PN_ARRAYSIZE(Config->CodecPriority) && Config->CodecPriority[i]; i++)
        if (Config->CodecPriority[i] == Codec) return i;
    return 0xFFFFFFFFu;
}

static inline BOOLEAN PnRequestDelayReporting(ULONG ConfigFlags, ULONG RemoteServiceCaps, USHORT RemoteVersion)
{
    return (ConfigFlags & PNA2DP_CFG_DELAY_REPORTING) && (RemoteServiceCaps & PNA2DP_SVC_DELAY_REPORTING) && RemoteVersion >= 0x0103;
}

/* ---- Stream format of a configured stream vs the audio engine -------------------- */
typedef enum _PN_FORMAT_ACTION {
    PnFormatKeep = 0,
    PnFormatReconfigure,          /* same codec, new rate/mode: (SUSPEND,) RECONFIGURE */
    PnFormatReselect,             /* the sink cannot carry the engine rate with this codec */
    PnFormatWaitOpenReconfigure   /* corrected transport: receiver-configured, wait for its OPEN */
} PN_FORMAT_ACTION;

typedef struct _PN_FORMAT_INPUT {
    BOOLEAN                  ActiveValid;
    const A2DP_CODEC_CONFIG *Active;
    const AVDTP_REMOTE_SEP  *Remote;        /* SEP of the current stream, NULL if unknown */
    const PNA2DP_CONFIG     *Config;
    BOOLEAN                  AudioStreamOpen;
    ULONG                    AudioRate;
    ULONG                    AudioValidBits;
    BOOLEAN                  Conservative;
    AVDTP_STREAM_STATE       StreamState;
    BOOLEAN                  RemoteConfigured;   /* corrected transport: the receiver configured it */
    ULONG                    RateConvert;        /* PN_RATECONV_*: engine rates the pump bridges (C8, #1) */
    BOOLEAN                  Streamed;           /* the stream carried audio (pump started) since it was configured */
} PN_FORMAT_INPUT;

/* TRUE when PnPlanStreamFormat keeps the stream at its own rate and the pump bridges the engine
 * rate to it (PN_RATECONV_ANY, 0.1.0.22 #1): no RECONFIGURE when Windows switches between 44.1
 * and 48 kHz on a stream that has already played. A stream that has carried no audio since it was
 * configured (the receiver's own SET_CONFIGURATION, or ours with no audio open) is not bridged:
 * a RECONFIGURE before its first START interrupts nothing, so it goes to the engine's own rate
 * (review #1, #3, #4). A hi-res engine rate (above 48 kHz) the sink takes natively still gets its
 * own stream. A bridged stream stays at its rate until it is configured again (codec upgrade,
 * renegotiation, reconnect). */
static inline BOOLEAN PnStreamBridged(const PN_FORMAT_INPUT *In, BOOLEAN Parsed, ULONG SinkRateMask)
{
    ULONG engine = In->AudioRate, active = In->Active->SampleRate;
    BOOLEAN native = Parsed && (SinkRateMask & A2dpHzToRateMask(engine));
    return In->RateConvert >= PN_RATECONV_ANY && In->Streamed && engine != active && PnRateConvertible(engine, active, In->RateConvert) &&
           !(native && engine > 48000);
}

/* The stream format at one stream rate (PnPlanStreamFormat). */
static inline PN_FORMAT_ACTION PnPlanStreamFormatAt(const PN_FORMAT_INPUT *In, BOOLEAN Parsed, const A2DP_CODEC_CAPS *Caps, ULONG Rate,
                                                    A2DP_CODEC_CONFIG *Cfg)
{
    *Cfg = *In->Active;
    A2DP_CODEC_CAPS caps = *Caps;
    BOOLEAN selected = Parsed && (caps.RateMask & A2dpHzToRateMask(Rate));
    if (selected && Cfg->Codec == PNA2DP_CODEC_SBC) {
        caps.RateMask &= A2dpHzToRateMask(Rate);
        selected = A2dpSelectConfig(&caps, In->Config, Rate, In->AudioValidBits, Cfg);
        if (selected && In->Conservative) selected = A2dpConstrainBasicRadio(Cfg);
    } else if (selected) {
        Cfg->SampleRate = Rate;
        A2dpSerializeConfig(Cfg);
    }
    if (selected && PnStreamFormatNeedsUpdate(In->Active->Codec, In->Active->SampleRate, In->Active->ChannelMode,
                                              Cfg->SampleRate, Cfg->ChannelMode))
        return (In->RemoteConfigured && In->StreamState == AvdtpStateConfigured) ? PnFormatWaitOpenReconfigure : PnFormatReconfigure;
    if (!selected && In->Active->SampleRate != Rate) return PnFormatReselect;
    return PnFormatKeep;
}

/* A receiver-initiated SBC stream may ignore the saved channel preference: select from
 * the sink's actual capabilities before comparing stream formats; in-band bitrate
 * reductions never need a reconfiguration. RECONFIGURE needs an Open stream: a stream the
 * receiver configured waits for the receiver's OPEN (T3) instead of being dropped, one we
 * configured ourselves is aborted and configured again. A bridged stream (PnStreamBridged) that
 * needs a RECONFIGURE anyway, for a channel-mode preference, is reconfigured to the engine's own
 * stream rate instead: the same interruption, and the conversion ends (review #1, #3). With
 * PN_RATECONV_NONE or _HALF the decisions are those of 0.1.0.21. */
static inline PN_FORMAT_ACTION PnPlanStreamFormat(const PN_FORMAT_INPUT *In, A2DP_CODEC_CONFIG *Cfg)
{
    AVDTP_STREAM_STATE st = In->StreamState;
    if (!In->ActiveValid || !In->AudioStreamOpen ||
        (st != AvdtpStateOpen && st != AvdtpStateStreaming && st != AvdtpStateConfigured)) return PnFormatKeep;
    *Cfg = *In->Active;
    const AVDTP_REMOTE_SEP *r = In->Remote;
    A2DP_CODEC_CAPS caps = {};
    BOOLEAN parsed = r && A2dpParseCaps(r->MediaCodecType, r->CodecCaps, r->CodecCapsLength, &caps, NULL, NULL);
    ULONG rate = parsed ? PnStreamRateFor(caps.RateMask, In->AudioRate, In->RateConvert) : In->AudioRate;
    if (!PnStreamBridged(In, parsed, caps.RateMask)) return PnPlanStreamFormatAt(In, parsed, &caps, rate, Cfg);
    PN_FORMAT_ACTION bridged = PnPlanStreamFormatAt(In, parsed, &caps, In->Active->SampleRate, Cfg);
    if (bridged != PnFormatReconfigure && bridged != PnFormatWaitOpenReconfigure) return bridged;
    A2DP_CODEC_CONFIG own;
    PN_FORMAT_ACTION action = PnPlanStreamFormatAt(In, parsed, &caps, rate, &own);
    if (action != PnFormatReconfigure && action != PnFormatWaitOpenReconfigure) return bridged;   /* the engine rate fails: as bridged */
    *Cfg = own;
    return action;
}

/* ---- Media pump vs stream state ------------------------------------------------- */
/* May media flow in this stream state? Only while Streaming (T2): an Open stream is
 * suspended, by us or by the receiver. */
static inline BOOLEAN PnPumpMayRun(AVDTP_STREAM_STATE St)
{
    return St == AvdtpStateStreaming;
}

/* PNA2DP_STATE_* reported for the AVDTP stream state. */
static inline ULONG PnStatusState(AVDTP_STREAM_STATE St, BOOLEAN SignallingUp, BOOLEAN PumpStreaming)
{
    (void)PumpStreaming;   /* the pump never runs outside Streaming */
    switch (St) {
    case AvdtpStateIdle: return SignallingUp ? PNA2DP_STATE_DISCOVERING : PNA2DP_STATE_IDLE;
    case AvdtpStateConfigured:
    case AvdtpStateOpening: return PNA2DP_STATE_CONFIGURED;
    case AvdtpStateOpen: return PNA2DP_STATE_OPEN;
    case AvdtpStateStreaming: return PNA2DP_STATE_STREAMING;
    default: return PNA2DP_STATE_CLOSING;
    }
}

/* ---- Reconcile plan --------------------------------------------------------------- */
typedef enum _PN_RECONCILE_STEP {
    PnStepNone = 0,
    PnStepStopPump,            /* PumpStopStream */
    PnStepResetAttempts,       /* ReconnectAttempts = 0 */
    PnStepResetSession,        /* power resume: stop pump, drop signalling, forget the configuration */
    PnStepCloseForHostLoss,    /* the codec host went away under its stream: close to renegotiate (SBC in the kernel) */
    PnStepEnsureSignalling,
    PnStepEnsureDiscovered,
    PnStepCheckFormat,         /* PnPlanStreamFormat on the live stream, then act on it */
    PnStepEnsureOpen,          /* SET_CONFIGURATION / OPEN as needed; PENDING ends the pass quietly */
    PnStepStartIfOpen,         /* live state Open: wake the ACL, START */
    PnStepStartPump,           /* live state PnPumpMayRun: start the pump, notify; else end the pass */
    PnStepSuspend,             /* SUSPEND */
    PnStepNotifyState,
    /* corrected transport */
    PnStepAbortStream,         /* media stall: probe the receiver, keep / ABORT / drop / yield (PnStallProbeAction) */
    PnStepWaitRemoteOpen,      /* receiver configured the stream: give it time to send OPEN */
    PnStepUpgradeCodec,        /* switch a receiver-chosen codec once to a better one */
    PnStepArmSuspendTimer,     /* receiver started while we are paused/stopped */
    PnStepLinkLost,            /* every channel is gone: reset the session */
    PnStepCount
} PN_RECONCILE_STEP;

typedef struct _PN_RECONCILE_INPUT {
    BOOLEAN            Removing;
    BOOLEAN            PowerSuspended;
    BOOLEAN            ResetBudget;          /* consumed ResetReconnectBudget */
    BOOLEAN            PowerResumePending;   /* consumed PowerResumePending */
    BOOLEAN            AudioRun;             /* AudioRequest == PnAudioRun */
    BOOLEAN            AudioStreamOpen;
    BOOLEAN            ScoActive;
    BOOLEAN            SuspendDue;
    ULONG              ConfigFlags;
    AVDTP_STREAM_STATE StreamState;
    BOOLEAN            PumpStreaming;
    BOOLEAN            PumpOwnerHost;
    BOOLEAN            HostPresent;          /* codec host registered with any codec */
    /* corrected transport */
    BOOLEAN            SignallingUp;
    BOOLEAN            RemoteSuspended;      /* receiver SUSPENDed our stream */
    BOOLEAN            LocalStartRequested;  /* playback (re)started since then */
    ULONG              RemoteSuspendAgeMs;
    BOOLEAN            RemoteSuspendRetried; /* an unprompted START already went out in this suspend episode */
    BOOLEAN            RemoteStarted;        /* receiver STARTed the stream */
    BOOLEAN            RemoteConfigured;     /* receiver configured the current stream */
    ULONG              RemoteConfiguredAgeMs;
    BOOLEAN            BetterCodecAvailable; /* a higher-priority codec may fit (unknown before discovery: the step re-checks) */
    BOOLEAN            CodecUpgradeTried;
    BOOLEAN            RemoteSepsUnknown;    /* not discovered in this session: an upgrade cannot decide yet */
    BOOLEAN            MediaStalled;         /* C4: writes outstanding, none completing */
    BOOLEAN            LinkLost;             /* ACL down for our remote address */
    ULONG              SetupFailures;
    BOOLEAN            UserDisconnected;     /* C7: Windows disconnected the receiver; no automatic connect */
} PN_RECONCILE_INPUT;

#define PN_RECONCILE_MAX_STEPS 20
typedef struct _PN_RECONCILE_PLAN {
    ULONG Count;
    UCHAR Steps[PN_RECONCILE_MAX_STEPS];
    ULONG RetryDelayMs;          /* schedule another pass (0: none) */
} PN_RECONCILE_PLAN;

static inline VOID PnPlanAdd(PN_RECONCILE_PLAN *P, PN_RECONCILE_STEP Step)
{
    if (P->Count < PN_RECONCILE_MAX_STEPS) P->Steps[P->Count++] = (UCHAR)Step;
}

static inline BOOLEAN PnPlanHas(const PN_RECONCILE_PLAN *P, PN_RECONCILE_STEP Step)
{
    for (ULONG i = 0; i < P->Count; i++) if (P->Steps[i] == Step) return TRUE;
    return FALSE;
}

static inline BOOLEAN PnReconcileScoBlocks(const PN_RECONCILE_INPUT *In)
{
    return In->ScoActive && (In->ConfigFlags & PNA2DP_CFG_SUSPEND_ON_SCO);
}

/* Audio is wanted on the receiver (the planner's "want"). After a user disconnect (C7)
 * nothing connects until ONESHOT_RECONNECT, a new local playback start or an incoming
 * signalling connection from the receiver clears UserDisconnected (PnIncomingLiftsUserDisconnect). */
static inline BOOLEAN PnReconcileWantsAudio(const PN_RECONCILE_INPUT *In)
{
    return !In->Removing && !In->PowerSuspended && In->AudioRun && In->AudioStreamOpen && !PnReconcileScoBlocks(In) &&
           !In->UserDisconnected;
}

/* T2: after a receiver SUSPEND no START of ours until a new local playback start, a
 * receiver START or this back-off (a multipoint sink serving another phone). The back-off
 * sends one START per suspend episode, for a sink that suspends for no reason: a sink
 * that refuses it or suspends again is serving another source and is left alone until
 * playback starts again here or it STARTs itself (review round 3 #3). A suspend this long
 * after that START begins a new episode. */
#define PN_REMOTE_SUSPEND_BACKOFF_MS   8000u
#define PN_REMOTE_SUSPEND_EPISODE_MS  60000u

/* T2 (review round 4 #7): audio that resumes inside an endpoint that stayed in RUN (a pause
 * on the receiver's remote: the player renders silence, no RUN edge) counts as a local
 * playback start. Resumed: at least PN_RESUME_SILENCE_MS of silence while the receiver
 * keeps our stream suspended, then signal. Audio that never paused (a multipoint sink
 * serving another source while Windows keeps playing) never counts. Called per wave tick. */
#define PN_RESUME_SILENCE_MS   1000u
typedef struct _PN_RESUME_DETECT {
    ULONG   SilentUs;
    BOOLEAN Armed;
} PN_RESUME_DETECT;

static inline BOOLEAN PnResumeDetect(PN_RESUME_DETECT *R, BOOLEAN Suspended, BOOLEAN Silent, ULONG ElapsedUs)
{
    if (!Suspended) { R->SilentUs = 0; R->Armed = FALSE; return FALSE; }
    if (Silent) {
        R->SilentUs = R->SilentUs > 0xFFFFFFFFu - ElapsedUs ? 0xFFFFFFFFu : R->SilentUs + ElapsedUs;
        if (R->SilentUs >= PN_RESUME_SILENCE_MS * 1000u) R->Armed = TRUE;
        return FALSE;
    }
    R->SilentUs = 0;
    if (!R->Armed) return FALSE;
    R->Armed = FALSE;
    return TRUE;
}

/* A receiver SUSPEND arrives: does the unprompted START sent SinceRetryMs ago still belong
 * to this episode (no further timed START)? */
static inline BOOLEAN PnRemoteSuspendKeepsRetried(BOOLEAN Retried, ULONG SinceRetryMs)
{
    return Retried && SinceRetryMs < PN_REMOTE_SUSPEND_EPISODE_MS;
}
/* T3/T9: after a receiver SET_CONFIGURATION, time for its own OPEN before we send ours. */
#define PN_REMOTE_OPEN_WAIT_MS         1500u
#define PN_PLAN_MIN_RETRY_MS            250u

static inline ULONG PnPlanWaitLeft(ULONG TotalMs, ULONG AgeMs)
{
    ULONG left = AgeMs < TotalMs ? TotalMs - AgeMs : 0;
    return left < PN_PLAN_MIN_RETRY_MS ? PN_PLAN_MIN_RETRY_MS : left;
}

/* T3/T9: wait left before our OPEN of a stream the receiver configured, 0 when ours may go
 * out. Checked by the planner and again at the OPEN itself: the acceptor answers a
 * receiver SET_CONFIGURATION during our discovery, after the pass's snapshot (review
 * round 5 #3). */
static inline ULONG PnRemoteOpenWaitMs(BOOLEAN RemoteConfigured, AVDTP_STREAM_STATE St, ULONG AgeMs)
{
    if (!RemoteConfigured || St != AvdtpStateConfigured || AgeMs >= PN_REMOTE_OPEN_WAIT_MS) return 0;
    return PnPlanWaitLeft(PN_REMOTE_OPEN_WAIT_MS, AgeMs);
}

/* Make reality match the audio request. The plan is ordered; the executor stops at the
 * first step that ends the pass (failure, PENDING, nothing to stream). RetryDelayMs asks
 * for another pass; the executor also arms one whenever a pass ends with audio wanted
 * but not streaming (T4). */
static inline VOID PnPlanReconcile(const PN_RECONCILE_INPUT *In, PN_RECONCILE_PLAN *P)
{
    P->Count = 0;
    P->RetryDelayMs = 0;
    if (In->Removing) return;
    if (In->PowerSuspended) { PnPlanAdd(P, PnStepStopPump); return; }
    if (In->ResetBudget) PnPlanAdd(P, PnStepResetAttempts);
    AVDTP_STREAM_STATE st = In->StreamState;
    BOOLEAN pump = In->PumpStreaming;
    if (In->PowerResumePending) { PnPlanAdd(P, PnStepResetSession); st = AvdtpStateIdle; pump = FALSE; }
    BOOLEAN scoBlock = PnReconcileScoBlocks(In);
    BOOLEAN want = In->AudioRun && In->AudioStreamOpen && !scoBlock && !In->UserDisconnected;

    /* T13: the ACL to the receiver is gone, or media writes stopped completing */
    if (In->LinkLost) {
        if (pump) PnPlanAdd(P, PnStepStopPump);
        PnPlanAdd(P, PnStepLinkLost);
        if (want) P->RetryDelayMs = PnSetupRetryDelayMs(In->SetupFailures);
        return;
    }
    if (In->MediaStalled) {
        /* the step probes first and stops the pump only to tear the stream down; the retry
         * after a teardown is the T4 one, from the failure count the step leaves (a count
         * taken here may be stale: review round 3 #6) */
        PnPlanAdd(P, PnStepAbortStream);
        return;
    }

    /* T10 / G9: a better codec than the stream's, once per signalling session (no ping-pong
     * when a recovered codec host fails again). The snapshot reports BetterCodecAvailable
     * for a receiver-configured stream (unknown before discovery) and for one of ours whose
     * codec now ranks below what the chooser picks (a host codec came back from quarantine);
     * the switch happens only before a START or at a pause, never mid-stream. */
    BOOLEAN upgrade = In->BetterCodecAvailable && !In->CodecUpgradeTried;
    if (want) {
        /* the remote closed, aborted or suspended the stream under a running pump: stop it
         * before anything is reconfigured, so the next start sets the encoder up again */
        if (pump && !PnPumpMayRun(st)) { PnPlanAdd(P, PnStepStopPump); pump = FALSE; }
        /* the codec host went away while it was encoding: renegotiate (SBC in the kernel) */
        if (pump && In->PumpOwnerHost && !In->HostPresent) {
            PnPlanAdd(P, PnStepStopPump);
            PnPlanAdd(P, PnStepCloseForHostLoss);
            pump = FALSE;
        }
        /* T2: the receiver suspended us; respect it. After the one timed START of this
         * episode only re-check, never START again (review round 3 #3). */
        if (In->RemoteSuspended && !In->LocalStartRequested && st == AvdtpStateOpen &&
            (In->RemoteSuspendRetried || In->RemoteSuspendAgeMs < PN_REMOTE_SUSPEND_BACKOFF_MS)) {
            P->RetryDelayMs = In->RemoteSuspendRetried ? PN_REMOTE_SUSPEND_BACKOFF_MS
                                                       : PnPlanWaitLeft(PN_REMOTE_SUSPEND_BACKOFF_MS, In->RemoteSuspendAgeMs);
            return;
        }
        /* T3/T9: the receiver configured the stream; its OPEN normally follows */
        ULONG openWait = PnRemoteOpenWaitMs(In->RemoteConfigured, st, In->RemoteConfiguredAgeMs);
        if (openWait) {
            PnPlanAdd(P, PnStepWaitRemoteOpen);
            P->RetryDelayMs = openWait;
            return;
        }
        PnPlanAdd(P, PnStepEnsureSignalling);
        PnPlanAdd(P, PnStepEnsureDiscovered);
        PnPlanAdd(P, PnStepCheckFormat);
        /* T10: a receiver-chosen codec, before we START */
        if (upgrade && (st == AvdtpStateConfigured || st == AvdtpStateOpen)) PnPlanAdd(P, PnStepUpgradeCodec);
        PnPlanAdd(P, PnStepEnsureOpen);
        PnPlanAdd(P, PnStepStartIfOpen);
        PnPlanAdd(P, PnStepStartPump);
    } else {
        BOOLEAN stop = In->SuspendDue || scoBlock || !In->AudioStreamOpen;
        if (pump && (st != AvdtpStateStreaming || stop)) PnPlanAdd(P, PnStepStopPump);
        if (st == AvdtpStateStreaming && stop) {
            PnPlanAdd(P, PnStepSuspend);
            PnPlanAdd(P, PnStepNotifyState);
        } else if (st == AvdtpStateStreaming && In->RemoteStarted) {
            /* T2: the receiver started while we are paused or stopped */
            PnPlanAdd(P, PnStepArmSuspendTimer);
        }
        /* T10: a receiver-chosen codec that was already streaming, at the pause; only once
         * the receiver's SEPs are known (no discovery while paused), so the one try per
         * session is not spent on an empty list (review round 2 #2) */
        if (upgrade && !In->RemoteSepsUnknown && In->AudioStreamOpen && !scoBlock && (st == AvdtpStateOpen || (st == AvdtpStateStreaming && stop)))
            PnPlanAdd(P, PnStepUpgradeCodec);
    }
}

/* What to do when a step of the plan fails. Status is the step's NTSTATUS. */
typedef struct _PN_SETUP_FAILURE {
    BOOLEAN NotifyError;
    BOOLEAN SuspendIfStreaming;
    BOOLEAN AbortStream;            /* corrected transport */
    BOOLEAN Disconnect;
    BOOLEAN BumpReconnectAttempts;
    ULONG   RetryDelayMs;           /* 0: no retry */
} PN_SETUP_FAILURE;

typedef struct _PN_FAILURE_INPUT {
    PN_RECONCILE_STEP Step;
    LONG              Status;
    ULONG             ReconnectAttempts;     /* live value */
    ULONG             ReconnectDelayMs;      /* PNA2DP_CONFIG::ReconnectDelayMs */
    /* corrected transport */
    ULONG             SetupFailures;
    ULONG             ConsecutiveTimeouts;   /* transaction timeouts before this one */
} PN_FAILURE_INPUT;

/* T4: every failed step retries once, after the capped exponential back-off of the
 * setup-failure counter (recovery_policy.h), which only continuous streaming resets.
 * A transaction timeout ABORTs the stream first; signalling is dropped only on the
 * second consecutive timeout (or when it is already gone). */
static inline VOID PnReconcileOnFailure(const PN_FAILURE_INPUT *In, PN_SETUP_FAILURE *Out)
{
    Out->NotifyError = TRUE;
    Out->SuspendIfStreaming = FALSE;
    Out->AbortStream = FALSE;
    Out->Disconnect = FALSE;
    Out->BumpReconnectAttempts = FALSE;
    Out->RetryDelayMs = PnSetupRetryDelayMs(In->SetupFailures);
    if (In->Step == PnStepEnsureSignalling) {
        ULONG reconnect = PnReconnectDelayMs(In->ReconnectAttempts, In->ReconnectDelayMs);
        if (reconnect > Out->RetryDelayMs) Out->RetryDelayMs = reconnect;
        Out->BumpReconnectAttempts = TRUE;
        return;
    }
    if (In->Step == PnStepStartPump) Out->SuspendIfStreaming = TRUE;
    if (In->Status == PN_ST_CONNECTION_DISCONNECTED) Out->Disconnect = TRUE;
    else if (In->Status == PN_ST_IO_TIMEOUT) {
        if (In->ConsecutiveTimeouts == 0) Out->AbortStream = TRUE;
        else Out->Disconnect = TRUE;
    }
}

/* SET_CONFIGURATION was refused. */
typedef enum _PN_SETCONFIG_ACTION {
    PnSetConfigFail = 0,
    PnSetConfigRetryWithoutDelay,   /* same request without delay reporting */
    PnSetConfigRediscover,          /* corrected transport: SEP_IN_USE -> one fresh DISCOVER */
    PnSetConfigTryNext              /* corrected transport: remember the pair, next codec */
} PN_SETCONFIG_ACTION;

/* Only an answer from the receiver is acted on here; timeouts and a lost channel go to
 * PnReconcileOnFailure. SEP_IN_USE: our cached in-use bits are stale, DISCOVER once (T1).
 * Any other refusal: remember the (codec, SEID) pair and take the next codec (T4). */
static inline PN_SETCONFIG_ACTION PnSetConfigFailureAction(LONG Status, UCHAR ErrorCode, BOOLEAN DelayRequested, BOOLEAN Rediscovered)
{
    if (Status != PN_ST_DEVICE_PROTOCOL_ERROR && Status != PN_ST_NOT_SUPPORTED) return PnSetConfigFail;
    if (ErrorCode == AVDTP_ERR_SEP_IN_USE) return Rediscovered ? PnSetConfigTryNext : PnSetConfigRediscover;
    if (DelayRequested) return PnSetConfigRetryWithoutDelay;
    return PnSetConfigTryNext;
}

/* OPEN was refused or timed out. */
typedef enum _PN_OPEN_ACTION {
    PnOpenFailKeep = 0,     /* leave the configuration */
    PnOpenFailAbort,        /* the sink no longer has it: ABORT to Idle, configure again */
    PnOpenFailCollision     /* corrected transport: both sides OPENed at once; retry after the back-off */
} PN_OPEN_ACTION;

/* BAD_STATE is a collision only when our acceptor refused the receiver's OPEN while ours
 * was outstanding (ReceiverOpenRefused), and only once in a row (Collisions: earlier
 * consecutive collisions). A sink whose SEP went back to Idle (BlueZ, Fluoride after a
 * lost transport) answers BAD_STATE as well: ABORT and configure again (review #3). */
static inline PN_OPEN_ACTION PnOpenFailureAction(LONG Status, UCHAR ErrorCode, BOOLEAN ReceiverOpenRefused, ULONG Collisions)
{
    if (Status == PN_ST_DEVICE_PROTOCOL_ERROR && ErrorCode == AVDTP_ERR_BAD_STATE && ReceiverOpenRefused && Collisions == 0)
        return PnOpenFailCollision;
    return (Status == PN_ST_DEVICE_PROTOCOL_ERROR || Status == PN_ST_NOT_SUPPORTED) ? PnOpenFailAbort : PnOpenFailKeep;
}

/* START came back rejected: a BAD_STATE reject while the receiver itself started the
 * stream is a collision that already reached the goal. */
static inline BOOLEAN PnStartRejectIsCollision(LONG Status, UCHAR ErrorCode, AVDTP_STREAM_STATE StateNow)
{
    return Status == PN_ST_DEVICE_PROTOCOL_ERROR && ErrorCode == AVDTP_ERR_BAD_STATE && StateNow == AvdtpStateStreaming;
}

/* Our media channel connect failed after OPEN was accepted. */
typedef enum _PN_MEDIA_CONNECT_ACTION {
    PnMediaConnectAbort = 0,
    PnMediaConnectAdopt,         /* corrected transport: the receiver opened it */
    PnMediaConnectReset          /* a new signalling session resets the stream: nothing to send */
} PN_MEDIA_CONNECT_ACTION;

/* Our connect fails (INVALID_DEVICE_STATE) when the receiver opened the transport first:
 * that channel completes the open (T9). ResetPending: the connect was cancelled for a new
 * signalling session, whose readers wait for the reset; an ABORT there could not be
 * answered (review #1). */
static inline PN_MEDIA_CONNECT_ACTION PnMediaConnectFailureAction(LONG Status, BOOLEAN RemoteOpenedMedia, BOOLEAN ResetPending)
{
    (void)Status;
    if (ResetPending) return PnMediaConnectReset;
    return RemoteOpenedMedia ? PnMediaConnectAdopt : PnMediaConnectAbort;
}

/* An incoming L2CAP connection while signalling is up: is it our media transport? Only
 * once an OPEN was accepted (the receiver's: MediaExpected) or sent (ours: MediaExpected
 * from the caller, or Opening). A merely Configured stream has no transport yet (T9). */
static inline BOOLEAN PnAcceptIncomingAsMedia(BOOLEAN SignallingUp, BOOLEAN MediaUp, BOOLEAN MediaConnecting,
                                              BOOLEAN MediaExpected, AVDTP_STREAM_STATE St)
{
    return SignallingUp && !MediaUp && !MediaConnecting && (MediaExpected || St == AvdtpStateOpening);
}

/* What an incoming PSM 0x19 connection is to us (review round 4 #1): the signalling
 * channel, the media channel of our stream, or one we reject (our own signalling connect
 * runs, a second channel, media without an OPEN). */
typedef enum _PN_INCOMING_KIND {
    PnIncomingReject = 0,
    PnIncomingSignalling,
    PnIncomingMedia
} PN_INCOMING_KIND;

static inline PN_INCOMING_KIND PnIncomingKind(BOOLEAN SignallingUp, BOOLEAN SignallingConnecting, BOOLEAN MediaUp,
                                              BOOLEAN MediaConnecting, BOOLEAN MediaExpected, AVDTP_STREAM_STATE St)
{
    if (!SignallingUp && !SignallingConnecting) return PnIncomingSignalling;
    if (PnAcceptIncomingAsMedia(SignallingUp, MediaUp, MediaConnecting, MediaExpected, St)) return PnIncomingMedia;
    return PnIncomingReject;
}

/* C7: only the receiver connecting its signalling channel to us (accepted) lifts a user
 * disconnect and plugs the jack. A rejected channel, or the media channel of the session
 * the disconnect is about to end, must not cancel it (review round 4 #1). */
static inline BOOLEAN PnIncomingLiftsUserDisconnect(PN_INCOMING_KIND Kind, BOOLEAN Accepted)
{
    return Kind == PnIncomingSignalling && Accepted;
}

/* Closing a stream (local decision). */
typedef enum _PN_CLOSE_ACTION {
    PnCloseNothing = 0,
    PnCloseSendClose,        /* CLOSE, then drop the media channel */
    PnCloseLocalOnly,        /* drop locally without telling the sink */
    PnCloseSendAbort         /* corrected transport: ABORT a configured/opening stream */
} PN_CLOSE_ACTION;

/* CLOSE needs an Open stream; a Configured or Opening one is ABORTed, so the receiver's
 * SEP is free for our next SET_CONFIGURATION (T3). */
static inline PN_CLOSE_ACTION PnStreamCloseAction(AVDTP_STREAM_STATE St)
{
    if (St == AvdtpStateIdle) return PnCloseNothing;
    if (St == AvdtpStateOpen || St == AvdtpStateStreaming) return PnCloseSendClose;
    if (St == AvdtpStateConfigured || St == AvdtpStateOpening) return PnCloseSendAbort;
    return PnCloseLocalOnly;   /* already closing or aborting */
}

/* ---- Our command vs the receiver's (T9) -------------------------------------------- */
/* What our answered (or unanswered) command means for the stream. */
typedef enum _PN_INT_OUTCOME {
    PnIntApply = 0,     /* the stream is as we left it: apply our transition */
    PnIntReached,       /* the receiver's own command already took the stream where we wanted it */
    PnIntPending,       /* the receiver is completing the goal (its OPEN: its transport) */
    PnIntLost,          /* the stream moved elsewhere meanwhile: our answer no longer applies */
    PnIntFailed         /* refused / unanswered and the stream unchanged: report Status */
} PN_INT_OUTCOME;

/* Sent: the state our command was sent in (CLOSE / ABORT: the Closing / Aborting we set
 * before sending); Now: the state once the answer is in, after the acceptor applied the
 * receiver's commands. A collision that reached our goal (both STARTed: ours comes back
 * BAD_STATE) is success, not a reason to abort. */
static inline PN_INT_OUTCOME PnIntOutcome(UCHAR SignalId, LONG Status, AVDTP_STREAM_STATE Sent, AVDTP_STREAM_STATE Now)
{
    BOOLEAN ok = PnStOk(Status);
    switch (SignalId) {
    case AVDTP_START:
        if (Now == AvdtpStateStreaming) return PnIntReached;
        if (Now != Sent) return PnIntLost;
        return ok ? PnIntApply : PnIntFailed;
    case AVDTP_SUSPEND:
        if (Now == AvdtpStateOpen) return PnIntReached;
        if (Now != Sent) return PnIntLost;
        /* a refusal still ends our streaming: the receiver is not playing it either */
        return (ok || Status == PN_ST_DEVICE_PROTOCOL_ERROR) ? PnIntApply : PnIntFailed;
    case AVDTP_OPEN:
        if (Now == AvdtpStateOpen || Now == AvdtpStateStreaming) return PnIntReached;
        if (Now == AvdtpStateOpening) return PnIntPending;   /* the receiver's OPEN: its transport */
        if (Now != Sent) return PnIntLost;
        return ok ? PnIntApply : PnIntFailed;
    case AVDTP_CLOSE:
    case AVDTP_ABORT:
        /* tearing down: done whatever the answer, unless the receiver set up a new stream */
        if (Now == Sent) return PnIntApply;
        if (Now == AvdtpStateIdle) return PnIntReached;
        return PnIntLost;
    case AVDTP_RECONFIGURE:
        /* the receiver's START crossed ours: it judged our RECONFIGURE in Open (L2CAP keeps
         * order) and took it, so its stream runs the new configuration, as when its START
         * waited for our procedure (review round 5 #2). A refusal leaves the old one. */
        if (ok && Sent == AvdtpStateOpen && Now == AvdtpStateStreaming) return PnIntApply;
        if (Now != Sent) return PnIntLost;
        return ok ? PnIntApply : PnIntFailed;
    default:   /* SET_CONFIGURATION */
        if (Now != Sent) return PnIntLost;
        return ok ? PnIntApply : PnIntFailed;
    }
}

/* A media channel the receiver opened finished connecting (Ready) or failed. */
typedef enum _PN_MEDIA_UP_ACTION {
    PnMediaUpDrop = 0,      /* not ours: close it; an Opening stream falls back to Configured */
    PnMediaUpOpenStream,    /* the transport of an Opening stream: Open */
    PnMediaUpHold           /* our OPEN's answer is still on its way: AvdtpOpen adopts it */
} PN_MEDIA_UP_ACTION;

static inline PN_MEDIA_UP_ACTION PnMediaUpAction(BOOLEAN Ready, AVDTP_STREAM_STATE St, BOOLEAN OpenSent)
{
    if (!Ready) return PnMediaUpDrop;
    if (St == AvdtpStateOpening) return PnMediaUpOpenStream;
    if (St == AvdtpStateConfigured && OpenSent) return PnMediaUpHold;
    return PnMediaUpDrop;
}

/* Our OPEN was accepted: who provides the transport. */
typedef enum _PN_OPEN_TRANSPORT {
    PnOpenConnect = 0,      /* we connect the media channel */
    PnOpenAdoptReady,       /* the receiver's channel is connected and held: Open now */
    PnOpenAdoptPending      /* the receiver's channel is still being accepted: its event opens the stream */
} PN_OPEN_TRANSPORT;

/* RemoteMediaCurrent: the receiver's accepted channel is the media channel's current
 * connection and still connecting or connected. MediaUp alone may be left over from a
 * channel that is gone; adopting that would wait for a MEDIA_UP that never comes
 * (review round 3 #2). */
static inline PN_OPEN_TRANSPORT PnOpenTransport(BOOLEAN RemoteMediaUp, BOOLEAN RemoteMediaCurrent, BOOLEAN RemoteMediaHeld)
{
    if (!RemoteMediaUp || !RemoteMediaCurrent) return PnOpenConnect;
    return RemoteMediaHeld ? PnOpenAdoptReady : PnOpenAdoptPending;
}

/* The receiver's channel (see PnOpenTransport): accepted by us, still the current
 * connection, not closing. */
static inline BOOLEAN PnRemoteMediaCurrent(BOOLEAN RemoteInitiated, BOOLEAN AcceptGenCurrent, BOOLEAN ConnectingOrConnected)
{
    return RemoteInitiated && AcceptGenCurrent && ConnectingOrConnected;
}

/* Our media connect returned success: a transport only while the channel is still our
 * connected one (the receiver may have closed it right after it came up). */
static inline BOOLEAN PnOwnMediaLive(BOOLEAN Connected, BOOLEAN RemoteInitiated)
{
    return Connected && !RemoteInitiated;
}

/* Result of one transaction. Woken: the wait ended without a timeout; MessageType 0xFF:
 * no response was recorded. */
static inline LONG PnTransactStatus(BOOLEAN TimedOut, BOOLEAN SignallingUp, UCHAR MessageType, UCHAR ErrorCode)
{
    if (TimedOut) return PN_ST_IO_TIMEOUT;
    if (!SignallingUp) return PN_ST_CONNECTION_DISCONNECTED;
    if (MessageType == AVDTP_MSG_RESPONSE_ACCEPT) return 0;
    if (MessageType == 0xFF) return PN_ST_CANCELLED;   /* woken without an answer: a new session took over */
    if (MessageType == AVDTP_MSG_GENERAL_REJECT) return PN_ST_NOT_SUPPORTED;
    return ErrorCode == AVDTP_ERR_NOT_SUPPORTED_COMMAND ? PN_ST_NOT_SUPPORTED : PN_ST_DEVICE_PROTOCOL_ERROR;
}

/* Before one of our commands goes out. ResetPending: a new signalling session was accepted
 * while a stream of the old one existed and has not been reset yet. A command of the old
 * session's procedures would go out on the new channel, whose readers start only after
 * the reset, which waits for our command lock: it could only time out (review #1). */
static inline LONG PnTransactPrecheck(BOOLEAN SignallingUp, BOOLEAN ResetPending)
{
    if (!SignallingUp) return PN_ST_CONNECTION_DISCONNECTED;
    if (ResetPending) return PN_ST_CANCELLED;
    return 0;
}

/* A receiver command that arrived after the answer to our command (sequence AfterResponse)
 * must see that answer applied first; AppliedSeq counts the applied answers. */
static inline BOOLEAN PnAcpMustWait(ULONG AfterResponse, ULONG AppliedSeq)
{
    return (LONG)(AfterResponse - AppliedSeq) > 0;
}

/* Our OPEN connects the media channel, and our CLOSE / ABORT disconnects it, after the
 * transaction ended (T9): the stream is Opening / Closing / Aborting meanwhile. A receiver
 * command whose answer depends on the stream state waits for that (TransportBusy; bounded
 * by the caller) and is judged against Open / Idle, as when the command lock covered the
 * transport (review round 4 #2). ABORT, the receiver's way out, and state-free commands
 * are answered at once. */
static inline BOOLEAN PnAcpWaitsForTransport(UCHAR SignalId, BOOLEAN TransportBusy)
{
    if (!TransportBusy) return FALSE;
    switch (SignalId) {
    case AVDTP_DISCOVER:
    case AVDTP_SET_CONFIGURATION:
    case AVDTP_RECONFIGURE:
    case AVDTP_OPEN:
    case AVDTP_START:
    case AVDTP_CLOSE:
    case AVDTP_SUSPEND:
        return TRUE;
    default:
        return FALSE;
    }
}

/* The other direction: our procedure judges an answer only once the acceptor finished the
 * receiver's commands that arrived before it (AcpBefore: commands queued when the answer
 * arrived; AcpDone: commands the acceptor finished). A receiver OPEN or START that crossed
 * ours is then refused as the collision, or applied, before our BAD_STATE answer is judged,
 * whichever thread the scheduler runs first (review round 2 #1/#3). */
static inline BOOLEAN PnIntMustWaitForAcp(ULONG AcpBefore, ULONG AcpDone)
{
    return (LONG)(AcpBefore - AcpDone) > 0;
}

/* ---- Discovery -------------------------------------------------------------------- */
typedef enum _PN_CAPS_ACTION {
    PnCapsParse = 0,        /* parse the response, next SEP */
    PnCapsRetryBasic,       /* ask the same SEP again with GET_CAPABILITIES */
    PnCapsNextSep,
    PnCapsStop
} PN_CAPS_ACTION;

typedef struct _PN_CAPS_QUERY {
    BOOLEAN UseAll;         /* GET_ALL_CAPABILITIES (AVDTP 1.3) */
    BOOLEAN Downgraded;     /* fell back to GET_CAPABILITIES during this discovery, on evidence */
    BOOLEAN Retrying;       /* GET_ALL_CAPABILITIES got no usable answer: the basic query decides */
    USHORT  Version;        /* remote AVDTP version as we now believe it */
} PN_CAPS_QUERY;

static inline VOID PnCapsQueryStart(PN_CAPS_QUERY *Q, USHORT RemoteVersion, BOOLEAN KnownBasicOnly)
{
    Q->Version = RemoteVersion;
    Q->UseAll = RemoteVersion >= 0x0103 && !KnownBasicOnly;
    Q->Downgraded = FALSE;
    Q->Retrying = FALSE;
}

static inline UCHAR PnCapsQuerySignal(const PN_CAPS_QUERY *Q)
{
    return Q->UseAll ? AVDTP_GET_ALL_CAPABILITIES : AVDTP_GET_CAPABILITIES;
}

/* Status of the capability transaction for one SEP. GET_ALL_CAPABILITIES that is refused,
 * general-rejected or unanswered is asked again with GET_CAPABILITIES (T5: AVDTP 1.2 sinks
 * often ignore it). It is a downgrade (AVDTP 1.2 for the rest of the discovery; the caller
 * remembers it for the device) only on evidence: refused as a command (NOT_SUPPORTED), or
 * the basic query of the same SEP answering. When the basic query fails as well (a dead
 * link, a SEP the sink refuses either way) nothing is learnt and the next SEP is asked
 * with GET_ALL_CAPABILITIES again (review #4). Only a lost channel or an unanswered basic
 * query stops. */
static inline PN_CAPS_ACTION PnCapsQueryResult(PN_CAPS_QUERY *Q, LONG Status)
{
    BOOLEAN retrying = Q->Retrying;
    Q->Retrying = FALSE;
    if (PnStOk(Status)) {
        if (retrying) { Q->Downgraded = TRUE; Q->Version = 0x0102; }
        return PnCapsParse;
    }
    if (retrying) Q->UseAll = TRUE;   /* neither query answered: no evidence against GET_ALL */
    /* CANCELLED: a new signalling session ended the wait (T9) */
    if (Status == PN_ST_CONNECTION_DISCONNECTED || Status == PN_ST_CANCELLED) return PnCapsStop;
    if (Q->UseAll && !retrying) {
        Q->UseAll = FALSE;
        if (Status == PN_ST_NOT_SUPPORTED) { Q->Downgraded = TRUE; Q->Version = 0x0102; }
        else Q->Retrying = TRUE;
        return PnCapsRetryBasic;
    }
    if (Status == PN_ST_IO_TIMEOUT) return PnCapsStop;
    return PnCapsNextSep;
}

/* Discovery is done only when at least one SEP has usable capabilities (T5). */
static inline BOOLEAN PnDiscoveryValid(LONG LastStatus, ULONG SepCount, ULONG SepsWithCaps)
{
    (void)LastStatus;
    return SepCount > 0 && SepsWithCaps > 0;
}

/* SDP (for the AVDTP version) is queried whoever opened signalling, until one query
 * succeeds. A failed one is tried again in a later signalling session (review #5: the
 * receiver may still have been setting up its profiles), at most PN_SDP_MAX_ATTEMPTS times
 * per device, so a pass never waits out more than one failed SDP query per session. */
#define PN_SDP_MAX_ATTEMPTS 3u
static inline BOOLEAN PnShouldQuerySdp(BOOLEAN Succeeded, ULONG FailedAttempts, BOOLEAN FailedThisSession)
{
    return !Succeeded && FailedAttempts < PN_SDP_MAX_ATTEMPTS && !FailedThisSession;
}

/* ---- Power ------------------------------------------------------------------------ */
#define PN_PWR_BLOCK_WORK        0x01   /* PowerSuspended: workers only stop the pump */
#define PN_PWR_RESET_ON_RESUME   0x02   /* drop the Bluetooth session when D0 returns */
#define PN_PWR_CANCEL_RECONNECT  0x04
#define PN_PWR_QUEUE_WORK        0x08   /* the worker acts; the power callback never waits */
#define PN_PWR_SUSPEND_STREAM    0x10   /* corrected transport: stop the pump and SUSPEND, keep signalling */

/* Leaving D0. SystemWorking: the system stays in S0 (idle D3, Modern Standby): stop the
 * pump and SUSPEND from the worker, keep signalling (T11). Real system sleep: block work,
 * drop the session on resume unless it demonstrably survived. */
static inline ULONG PnPowerOnDx(BOOLEAN SystemWorking)
{
    if (SystemWorking) return PN_PWR_SUSPEND_STREAM | PN_PWR_QUEUE_WORK;
    return PN_PWR_BLOCK_WORK | PN_PWR_RESET_ON_RESUME | PN_PWR_CANCEL_RECONNECT | PN_PWR_QUEUE_WORK;
}

/* Back in D0 with a reset requested at Dx: drop the session only if signalling or the ACL
 * is actually gone. */
static inline BOOLEAN PnPowerResetOnResume(BOOLEAN ResetRequested, BOOLEAN SignallingUp, BOOLEAN AclUp)
{
    return ResetRequested && !(SignallingUp && AclUp);
}

/* ---- Link events ------------------------------------------------------------------ */
#define PN_ACL_FX_RESET_BUDGET    0x01
#define PN_ACL_FX_RECONNECT_SOON  0x02   /* give the headset 2.5 s to initiate AVDTP itself */
#define PN_ACL_FX_LINK_LOST       0x04   /* corrected transport: every channel is down */

/* An ACL-down HCI event for our remote address means every L2CAP channel is gone (T13). */
static inline ULONG PnAclEventAction(BOOLEAN Connected, BOOLEAN SignallingUp)
{
    if (!Connected) return PN_ACL_FX_LINK_LOST;
    return PN_ACL_FX_RESET_BUDGET | (SignallingUp ? 0 : PN_ACL_FX_RECONNECT_SOON);
}

/* ---- Receiver presence: the endpoint's jack (C6) ----------------------------------- */
/* The jack starts connected, as before (a stream open is what makes the device connect),
 * and is unplugged only by a user disconnect; any sign of the receiver plugs it again. Signalling closing while idle (the receiver's or our idle
 * close, a reset) is no such evidence and leaves it alone. Neither is the ACL going down
 * (switched off, standby, out of range): most receivers wait for the source to reconnect,
 * and nothing would while the endpoint is unplugged, so it stays plugged and the next
 * playback pages the receiver, as before C6; a user disconnect holds (review round 2 #7).
 * An unanswered media stall keeps it plugged too: it is the same outage seen before the ACL
 * supervision timeout, and unplugging then would move Windows' audio to another device
 * until the ACL going down plugged it back (review round 5 #5). */
typedef enum _PN_JACK_EVENT {
    PnJackAclUp = 0,           /* HCI ACL-connect event for our remote address */
    PnJackAclDown,             /* HCI ACL-disconnect event for our remote address */
    PnJackSignallingUp,        /* our signalling connect succeeded, or the receiver connected to us */
    PnJackSignallingClosed,    /* signalling closed without the ACL going down */
    PnJackStallAnswered,       /* media writes stalled, but the receiver answered the ABORT */
    PnJackStallUnanswered,     /* media writes stalled and the receiver no longer answers (kept: round 5 #5) */
    PnJackUserDisconnect,      /* KSPROPERTY_ONESHOT_DISCONNECT carried out */
    PnJackUserReconnect        /* KSPROPERTY_ONESHOT_RECONNECT: plugged when the connect succeeds */
} PN_JACK_EVENT;

#define PN_JACK_KEEP    0u
#define PN_JACK_PLUG    1u
#define PN_JACK_UNPLUG  2u

static inline ULONG PnJackAction(PN_JACK_EVENT Ev, BOOLEAN UserDisconnected)
{
    switch (Ev) {
    case PnJackAclUp:
    case PnJackSignallingUp:
        return PN_JACK_PLUG;
    case PnJackAclDown:
        return UserDisconnected ? PN_JACK_KEEP : PN_JACK_PLUG;
    case PnJackUserDisconnect:
        return PN_JACK_UNPLUG;
    default:
        return PN_JACK_KEEP;
    }
}

/* After a media stall whose probe went unanswered the session is dropped; while the ACL
 * stays up no ACL-up event comes, and with playback paused or stopped meanwhile no audio
 * pass retries. The connect worker keeps trying signalling with the reconnect back-off
 * until the receiver answers, the ACL goes down (the next playback pages) or the user
 * disconnects (review #11). With audio wanted Reconcile retries instead. */
static inline BOOLEAN PnRetryUnreachable(BOOLEAN WantAudio, BOOLEAN UserDisconnected, BOOLEAN Unreachable, BOOLEAN AclUp,
                                         BOOLEAN SignallingUp)
{
    return !WantAudio && !UserDisconnected && Unreachable && AclUp && !SignallingUp;
}

/* The ABORT after a media stall: any answer (accept or reject) shows the receiver is
 * still there; a timeout or a lost channel means it is unreachable. */
static inline PN_JACK_EVENT PnStallJackEvent(LONG AbortStatus)
{
    if (PnStOk(AbortStatus) || AbortStatus == PN_ST_DEVICE_PROTOCOL_ERROR || AbortStatus == PN_ST_NOT_SUPPORTED) return PnJackStallAnswered;
    return PnJackStallUnanswered;
}

/* C4 after a media stall (review round 3 #6). A 2-5 s radio fade stalls the writes while
 * the ACL survives; HEAD kept the stream and played on once the radio was back. So the
 * worker first sends a signalling probe (queued behind the stuck media writes on the same
 * ACL): answered and the writes complete again (within PN_STALL_PROBE_SETTLE_MS), the fade
 * is over and the stream stays; answered while the media writes are still stuck, the
 * transport is broken: ABORT and set up again; unanswered, the receiver is unreachable:
 * drop the channels (as after an unanswered ABORT). */
#define PN_STALL_PROBE_SETTLE_MS   500u

typedef enum _PN_STALL_ACTION {
    PnStallKeep = 0,
    PnStallAbort,
    PnStallDisconnect,
    PnStallYield         /* a new signalling session from the receiver: stop the pump, the reset ends the stream */
} PN_STALL_ACTION;

/* A probe or ABORT cancelled because the receiver opened a new signalling session
 * (CANCELLED), or such a session waiting to reset the stream (ResetPending), proves the
 * receiver is there: its new channel is neither dropped nor counted as a failure; the
 * stream reset's Idle transition queues the next pass (review round 5 #1). */
static inline BOOLEAN PnStallSessionReplaced(LONG Status, BOOLEAN ResetPending)
{
    return Status == PN_ST_CANCELLED || ResetPending;
}

static inline PN_STALL_ACTION PnStallProbeAction(LONG ProbeStatus, BOOLEAN StillStalled, BOOLEAN ResetPending)
{
    if (PnStallSessionReplaced(ProbeStatus, ResetPending)) return PnStallYield;
    if (PnStallJackEvent(ProbeStatus) != PnJackStallAnswered) return PnStallDisconnect;
    return StillStalled ? PnStallAbort : PnStallKeep;
}

/* ---- Sink delay ------------------------------------------------------------------- */
/* Receiver latency assumed when it does not report one (AVDTP delay reporting not
 * negotiated or no report yet). Heuristics, not measurements: typical decode + jitter
 * buffer + DAC of consumer sinks; low-latency aptX sinks run short buffers, LDAC/AAC
 * sinks long ones. Used only for presentation position and status. */
#define PN_SINK_DELAY_DEFAULT_US      150000u   /* SBC and unknown codecs */
#define PN_SINK_DELAY_AAC_US          180000u
#define PN_SINK_DELAY_APTX_US         150000u
#define PN_SINK_DELAY_APTX_HD_US      180000u
#define PN_SINK_DELAY_APTX_LL_US       60000u
#define PN_SINK_DELAY_LDAC_US         200000u

static inline ULONG PnDefaultSinkDelayUs(ULONG Codec)
{
    switch (Codec) {
    case PNA2DP_CODEC_AAC: return PN_SINK_DELAY_AAC_US;
    case PNA2DP_CODEC_APTX: return PN_SINK_DELAY_APTX_US;
    case PNA2DP_CODEC_APTX_HD: return PN_SINK_DELAY_APTX_HD_US;
    case PNA2DP_CODEC_APTX_LL: return PN_SINK_DELAY_APTX_LL_US;
    case PNA2DP_CODEC_LDAC: return PN_SINK_DELAY_LDAC_US;
    default: return PN_SINK_DELAY_DEFAULT_US;
    }
}

/* Plausibility floor of a receiver's delay report (0.1.0.25). No receiver presents audio
 * sooner than it decodes a frame and fills its jitter buffer: a report below the floor is
 * not its delay. The EDIFIER MR5 (LDAC 48 kHz) reports 25.0 ms right after it sets up the
 * stream itself and 364.0 ms after a stream restart; a microphone measured 265-295 ms both
 * times. A report below the floor is not used (the per-codec default applies) and the status
 * says so (SINK_DELAY_IGNORED). LDAC, AAC and aptX HD sinks buffer long; aptX, SBC and
 * unknown codecs less; aptX LL sinks run short buffers, so any report is believed. */
#define PN_SINK_DELAY_FLOOR_LONG_US    80000u   /* LDAC, AAC, aptX HD */
#define PN_SINK_DELAY_FLOOR_US         40000u   /* aptX, SBC and unknown codecs */

static inline ULONG PnMinPlausibleSinkDelayUs(ULONG Codec)
{
    switch (Codec) {
    case PNA2DP_CODEC_LDAC: case PNA2DP_CODEC_AAC: case PNA2DP_CODEC_APTX_HD: return PN_SINK_DELAY_FLOOR_LONG_US;
    case PNA2DP_CODEC_APTX_LL: return 0;
    default: return PN_SINK_DELAY_FLOOR_US;
    }
}

/* Where the applied sink delay comes from (PnSinkDelayDecide). */
typedef enum _PN_SINK_DELAY_SOURCE {
    PnSinkDelayNoStream = 0,
    PnSinkDelaySet,          /* the configured speaker delay (PNA2DP_CONFIG.SinkDelayMs) */
    PnSinkDelayReported,     /* the receiver's report */
    PnSinkDelayDefault       /* the per-codec default: no report, or one below the floor */
} PN_SINK_DELAY_SOURCE;

typedef struct _PN_SINK_DELAY_INPUT {
    BOOLEAN StreamActive;      /* an AVDTP stream exists (not Idle) */
    BOOLEAN ReportReceived;    /* a DELAYREPORT of this stream (T6) */
    ULONG   ReportedTenthsMs;  /* its value (16 bits on the wire) */
    ULONG   Codec;             /* PNA2DP_CODEC of the stream, NONE before it is known */
    ULONG   SetMs;             /* PNA2DP_CONFIG.SinkDelayMs: 0 = automatic */
    ULONG   SetCodec;          /* PNA2DP_CONFIG.SinkDelayCodec: 0 = any codec */
} PN_SINK_DELAY_INPUT;

typedef struct _PN_SINK_DELAY {
    ULONG                Us;             /* applied delay; 0 without a stream */
    ULONG                ReportedUs;     /* the receiver's report, 0 if none (PNA2DP_STATUS.SinkReportedDelayUs) */
    PN_SINK_DELAY_SOURCE Source;
    BOOLEAN              ReportIgnored;  /* the report is below the floor: not used (also when a set value applies) */
} PN_SINK_DELAY;

/* Sink delay for the current stream (C2; 0.1.0.25). ReportedTenthsMs: last AVDTP DELAYREPORT
 * of this stream. The acceptor stores a report only for the stream's own SEID and the value
 * is reset with every configuration (T6), so a received report belongs to this stream and is
 * used even when delay reporting was not negotiated: some sinks (the EDIFIER MR5 on its LDAC
 * SEP) report without offering the capability. Order: no stream -> 0; the configured speaker
 * delay when it is set for any codec or for this one; a report at or above the codec's floor;
 * else the per-codec default. */
static inline PN_SINK_DELAY PnSinkDelayDecide(const PN_SINK_DELAY_INPUT *In)
{
    PN_SINK_DELAY r = { 0, 0, PnSinkDelayNoStream, FALSE };
    if (!In->StreamActive) return r;
    if (In->ReportReceived) {
        r.ReportedUs = In->ReportedTenthsMs * 100;   /* <= 6553.5 ms */
        r.ReportIgnored = r.ReportedUs < PnMinPlausibleSinkDelayUs(In->Codec);
    }
    if (In->SetMs && (!In->SetCodec || In->SetCodec == In->Codec)) { r.Us = In->SetMs * 1000; r.Source = PnSinkDelaySet; }
    else if (In->ReportReceived && !r.ReportIgnored) { r.Us = r.ReportedUs; r.Source = PnSinkDelayReported; }
    else { r.Us = PnDefaultSinkDelayUs(In->Codec); r.Source = PnSinkDelayDefault; }
    return r;
}

/* The automatic decision (no speaker delay set). DelayNegotiated only documents the case. */
static inline ULONG PnSinkDelayUs(BOOLEAN StreamActive, BOOLEAN DelayNegotiated, BOOLEAN ReportReceived,
                                  ULONG ReportedTenthsMs, ULONG Codec)
{
    (void)DelayNegotiated;
    PN_SINK_DELAY_INPUT in = { StreamActive, ReportReceived, ReportedTenthsMs, Codec, 0, 0 };
    return PnSinkDelayDecide(&in).Us;
}

/* DeviceSinkDelayUs reads the speaker-delay setting lock-free at <= DISPATCH (the WaveRT
 * presentation position), so both words travel as one LONG: milliseconds in the low 16 bits
 * (validated <= 1000), the codec above them. */
static inline LONG PnSinkDelaySettingPack(ULONG SetMs, ULONG SetCodec)
{
    if (SetMs > 0xFFFFu) SetMs = 0xFFFFu;
    return (LONG)(SetMs | ((SetCodec & 0x7FFFu) << 16));
}

static inline ULONG PnSinkDelaySettingMs(LONG Setting) { return (ULONG)Setting & 0xFFFFu; }
static inline ULONG PnSinkDelaySettingCodec(LONG Setting) { return ((ULONG)Setting >> 16) & 0x7FFFu; }
