/*
 * pnlatency.h - acoustic output-latency measurement (the panel's "Measure with microphone" and
 * pna2dpctl measure-delay).
 *
 * The output plays short chirps; a microphone records them. Each chirp is compared with the
 * time the render stream's audio clock (IAudioClock::GetPosition, the driver's presentation
 * position, which already subtracts the driver's pipeline and speaker delay) says it plays.
 * lateMs > 0: the sound comes after the clock says it plays (the speaker delay the driver
 * applies is too small by that much); < 0: before (too large).
 *
 * Two parts, kept apart so the tests cover the analysis without any device:
 *   AnalyzeLatency     pure: a recording (chirp frames, clock samples, microphone samples with
 *                      their packet timestamps) in, per-chirp results and a verdict out.
 *   MeasureOutputLatency  WASAPI shared mode: plays the chirps, records, samples the clock,
 *                      then calls AnalyzeLatency. Plays sound: only on a user's explicit request.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace pn {

/* ---- the test signal and its schedule ---------------------------------------- */
struct LatencyOptions {
    /* Silence before the first chirp: the Bluetooth stream starts and the speaker fades in. */
    UINT32 leadInMs = 3000;
    /* Gaps between consecutive chirps (chirps = gaps + 1). NOT uniform: a lag that is wrong by
     * one gap then lines up at most one chirp with a neighbour's sound, so the single lag that
     * fits all chirps is unambiguous (uniform spacing aliased by +-1 period). Keep the gaps at
     * least 4 * toleranceMs apart from each other, and any two consecutive gaps together longer
     * than the lag search span (maxLagMs - minLagMs). */
    std::vector<UINT32> spacingMs = {900, 1150, 1000, 1300, 950, 1200, 1050};
    /* Silence after the last chirp, so its sound is recorded even maxLagMs late. */
    UINT32 tailMs = 2000;
    float amplitude = 0.5f;           /* of the chirp, on every channel (full scale 1.0) */
    /* analysis */
    int minLagMs = -500, maxLagMs = 1500;   /* the range searched for the run's single lag (FitLatencyLagRange) */
    UINT32 toleranceMs = 10;          /* per-chirp timing slack when scoring a lag */
    UINT32 refineMs = 20;             /* each chirp is located within the run's lag +- this */
    double clearMatch = 0.5;          /* normalized correlation at which a chirp counts as clear */
    unsigned minClear = 6;            /* clear chirps needed for a result (of 8) */
    double maxSpreadMs = 20;          /* largest max-min lateness of the clear chirps */
};

/* The chirp: 12 ms linear sweep 1 -> 7 kHz, Hann window, peak about 1.0, at the given sample rate. */
std::vector<float> ChirpSignal(UINT32 sampleRate);
/* Render frame index of each chirp's first frame: lead-in, then the gaps. */
std::vector<UINT64> LatencyChirpFrames(const LatencyOptions &options, UINT32 sampleRate);
/* Frames the run plays: through the last chirp and the tail (at least maxLagMs + 500 ms). */
UINT64 LatencyTotalFrames(const LatencyOptions &options, UINT32 sampleRate);
/* A driver that applies more speaker delay than the speaker has presents each chirp before it
 * sounds: lateMs down to -appliedMs (a Fixed 1000 ms on a 280 ms speaker: -720 ms). Above
 * 480 ms applied, the default range misses that, so it moves to [-(min(appliedMs, 1000) + 30),
 * 2000 ms after that]: the same 2000 ms span (below two consecutive gaps), holding every
 * lateness that gives a delay of 1..1000 ms for an applied delay up to 1000 ms. appliedMs: the
 * delay the driver is expected to apply during the run (pnctl.h ExpectedSinkDelayMs). */
void FitLatencyLagRange(LatencyOptions &options, UINT32 appliedMs);

/* ---- pure analysis ----------------------------------------------------------- */
struct LatencyRecording {
    UINT32 renderRate = 0;              /* render stream frames per second */
    std::vector<UINT64> chirpFrames;    /* LatencyChirpFrames at renderRate */
    /* IAudioClock::GetPosition samples: position in render frames (position * renderRate /
     * frequency) and the QPC time of that position in 100 ns units. Samples taken before the
     * position first advances (frame 0) carry no timing and are ignored. */
    struct ClockPoint { double frame; UINT64 qpc100ns; };
    std::vector<ClockPoint> clock;
    UINT32 captureRate = 0;
    std::vector<float> capture;         /* microphone, one channel, full scale 1.0 */
    /* One per capture packet: index of its first sample in capture[] and its u64QPCPosition
     * (100 ns units), in recording order. */
    struct CapturePacket { size_t index; UINT64 qpc100ns; };
    std::vector<CapturePacket> packets;
};

enum class LatencyVerdict {
    Ok,             /* enough clear chirps that agree: lateMs is the result */
    NotHeard,       /* too few chirps heard: volume or microphone distance */
    Inconsistent,   /* chirps heard but their delays disagree: noise, or the speaker changed its delay */
    NoClock,        /* the output's clock gave no presentation time for the chirps */
    NoRecording,    /* the microphone delivered no timed samples */
    Failed,         /* a Windows audio or device error (MeasureOutputLatency only) */
    Cancelled,      /* MeasureOutputLatency only */
};

struct LatencyChirpResult {
    bool timed = false;        /* the clock gave this chirp a presentation time */
    double presentedMs = 0;    /* that time, in ms after the first microphone packet */
    double lateMs = 0;         /* best match within the run's lag +- refineMs: heard minus presented */
    double match = 0;          /* normalized correlation there, 0..1 */
    bool clear = false;        /* match >= clearMatch: counted in the median and the spread */
    /* The best match within a wider window around the run's lag (never reaching a
     * neighbouring chirp): shows a chirp that was heard at a different delay. */
    double heardLateMs = 0, heardMatch = 0;
    bool heard = false;        /* heardMatch >= clearMatch */
};

struct LatencyResult {
    LatencyVerdict verdict = LatencyVerdict::Failed;
    std::wstring message;      /* one plain-language sentence or two, for every verdict */
    std::vector<LatencyChirpResult> chirps;
    double lagMs = 0;          /* the single lag that fits the run best (global search) */
    double lateMs = 0;         /* median lateness of the clear chirps (the result when Ok) */
    double spreadMs = 0;       /* max - min lateness of the clear chirps */
    unsigned clear = 0;        /* chirps clear at the run's lag */
    unsigned heard = 0;        /* chirps heard anywhere near it (>= clear) */
    unsigned total = 0;
    bool ok() const { return verdict == LatencyVerdict::Ok; }
};

/* Finds the one lag in [minLagMs, maxLagMs] whose summed matched-filter score over all chirps
 * is highest (chirp time = presentation time of its first frame from the nearest clock
 * sample), locates each chirp within that lag +- refineMs, and decides: Ok needs minClear clear
 * chirps with a spread of at most maxSpreadMs, and no two of the other chirps heard clearly
 * together at another delay (the speaker changed its delay during the run). A microphone that
 * records below 16 kHz is refused (Failed): the chirp reaches 7 kHz. Deterministic; no
 * devices, no COM. */
LatencyResult AnalyzeLatency(const LatencyRecording &recording, const LatencyOptions &options = LatencyOptions());
/* A microphone with several channels (a stereo microphone, an interface's inputs 1 and 2, which
 * Windows shows as one endpoint): AnalyzeLatency on each channel in place of recording.capture
 * (the same packets), keeping the best result: Ok first, then the most clear chirps, then the
 * highest summed match. A channel equal to an earlier one is skipped. channel (optional): the
 * index of the channel the result comes from. No channels: recording.capture as it is (0). */
LatencyResult AnalyzeLatencyChannels(const LatencyRecording &recording, const std::vector<std::vector<float>> &channels,
                                     const LatencyOptions &options = LatencyOptions(), unsigned *channel = nullptr);

/* "chirp  late by    match" table plus the summary sentence, for the command line. Lines end
 * in a bare newline (no carriage return). */
std::wstring LatencyReport(const LatencyResult &result);

/* ---- microphones --------------------------------------------------------------- */
struct CaptureEndpoint {
    std::wstring id;           /* IMMDevice id */
    std::wstring name;         /* friendly name: "Microphone (USB Audio Device)" */
    bool isDefault = false;    /* Windows' default recording device (console role) */
};
/* Active capture endpoints in Windows' order. Read-only; owns its COM scope (any apartment). */
HRESULT ListCaptureEndpoints(std::vector<CaptureEndpoint> &endpoints);
/* The endpoint to use for a name part: empty -> the default one (else the first); else an
 * exact case-insensitive name match, else the only name that contains the part (case-
 * insensitive). Returns the index, -1 when nothing matches, -2 when several names match. */
int MatchCaptureEndpoint(const std::vector<CaptureEndpoint> &endpoints, const std::wstring &namePart);

/* ---- the measurement ------------------------------------------------------------ */
enum class LatencyPhase {
    Starting,     /* streams opened, the output's clock has not moved yet */
    LeadIn,       /* the clock runs; silence before the first chirp */
    Chirps,       /* chirp (1-based) has been presented by the clock */
    Finishing,    /* reported once, after the last chirp, while the output stream still runs */
    Analysing,    /* streams stopped */
};
struct LatencyProgress {
    LatencyPhase phase = LatencyPhase::Starting;
    unsigned chirp = 0, chirps = 0;   /* chirps presented so far, of chirps */
    unsigned percent = 0;      /* 0..100 */
    bool step = false;         /* the first call for this phase / chirp (later calls repeat it) */
};
using LatencyProgressFn = std::function<void(const LatencyProgress &)>;
/* "Starting the speaker...", "Chirp 3 of 8...", "Analysing the recording..." */
std::wstring LatencyProgressText(const LatencyProgress &progress);

struct LatencyMeasurement {
    HRESULT hr = E_PENDING;    /* S_OK when the run completed and was analysed (see result.verdict) */
    LatencyResult result;      /* Failed / Cancelled carry their message here too */
    std::wstring renderName, captureName;
    UINT32 renderRate = 0, captureRate = 0;
    WORD captureChannels = 0, captureBits = 0;
    WORD captureChannel = 0;   /* the channel analysed, 0-based: the one that heard the chirps best (of the first 8) */
    bool captureFloat = false, captureRaw = false;   /* raw: the endpoint's effects were bypassed */
    ULONGLONG elapsedMs = 0;
};

/* Plays the chirps on renderEndpointId (shared mode, its float mix format) while recording
 * captureEndpointId (shared mode, raw where supported; every channel up to 8, analysed with
 * AnalyzeLatencyChannels), then analyses. A microphone that records below 16 kHz is refused
 * before anything plays. Pass the options FitLatencyLagRange fitted. Blocks for the length of
 * the run (about 13 s with the default options, plus the speaker's start): call it from a
 * worker thread. Owns its COM scope. cancel (optional) stops the run within about 20 ms.
 * progress (optional) is called on this thread with step set once per phase or chirp change,
 * and about every 250 ms in between with step clear. A caller that needs the driver's state
 * while the stream runs reads it on the step calls from LeadIn through Finishing (the stream
 * then still runs for the tail; AppliedDelayReadings::ReadOn); Analysing comes after both
 * streams stopped.
 * Both endpoint ids are resolved before anything is opened: an unknown id plays nothing. The
 * clock samples are taken from this run's own stream: start it in a fresh render run (no other
 * playback), since a delay change applied earlier in a running stream settles over seconds. */
LatencyMeasurement MeasureOutputLatency(const std::wstring &renderEndpointId, const std::wstring &captureEndpointId,
                                        const LatencyOptions &options = LatencyOptions(),
                                        const std::atomic<bool> *cancel = nullptr,
                                        const LatencyProgressFn &progress = LatencyProgressFn());

/* ---- the speaker delay a run measures -------------------------------------------- */
/* The driver's applied speaker delay (PNA2DP_STATUS.SinkDelayUs, driver 0.1.0.25) and the
 * stream's codec, read by the caller on the progress steps while the run plays (ReadOn: the
 * clock's start, every chirp, after the last chirp). The clock the run times the chirps with
 * already subtracts this delay, so the speaker's own delay is this value plus lateMs. */
struct AppliedDelayReadings {
    unsigned count = 0;
    UINT32 firstUs = 0, lastUs = 0, minUs = 0, maxUs = 0;
    ULONG codec = 0;               /* the stream's codec (PNA2DP_CODEC_*) at the first reading */
    bool codecChanged = false;
    bool finishing = false;        /* a reading was taken after the last chirp, the stream still running */
    void Add(UINT32 us, ULONG streamCodec, bool afterLastChirp);
    static bool ReadOn(const LatencyProgress &progress);      /* LeadIn, Chirps and Finishing steps */
    static bool Finishing(const LatencyProgress &progress);   /* the Finishing step */
};
struct SpeakerDelayMeasurement {
    bool ok = false;
    UINT32 delayMs = 0;            /* applied + lateMs, rounded: the speaker's delay (1..maxMs) */
    double appliedMs = 0;          /* the delay the driver applied during the run */
    std::wstring message;          /* "Measured 281 ms (8 of 8 chirps, spread 3 ms)." or why there is no value */
};
/* Pure. Needs an Ok result, readings that never changed (value and codec) including one after
 * the last chirp, a driver that reports the value (only 0: an older driver; a 0 among real
 * readings: the stream started or restarted during the run), and a result within 1..maxMs (the
 * driver's PN_CONFIG_MAX_SINK_DELAY_MS). Otherwise ok is false with a plain-language message:
 * the analysis' own message for a run that failed. */
SpeakerDelayMeasurement SpeakerDelayFromRun(const LatencyResult &result, const AppliedDelayReadings &applied, UINT32 maxMs = 1000);

} // namespace pn
