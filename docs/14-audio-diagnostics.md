# Audio diagnostics and live quality

The **Quality** page is available in both PN-A2DP and Windows mode and does not alter
codec, volume, driver or boot settings. Panels from 0.1.0.27 to 0.1.0.30 opened each
device on it; in application 0.1.0.34 a device opens on its first tab (Status, or Audio
in Windows mode) and Quality is the second tab.

## Check my audio

Press **Check my audio** to collect a fresh snapshot in a background worker. The
panel stays usable. Each worker owns its device identifiers and read-only control
handle; a result from an earlier device selection is discarded. No event listener
is attached by the diagnostic worker and no test tone is played.

The check reports device availability/problem codes, a pending driver restart,
the render endpoint matched to the exact device, multimedia-default selection,
volume/mute and mix format. PN mode adds control-interface responsiveness,
receiver codec support, registered encoder availability, codec-host state, SBC
fallback, the last recorded driver error and recent transport observations.
An unavailable query is shown as unknown. A non-default output is informational
because applications can select their own output. Kernel SBC does not require
the codec-host service. A last driver error may predate successful playback.

**Open suggested settings** opens the relevant PN page or Windows Sound/Bluetooth
settings for the first actionable problem. It does not change settings itself.
Results carry a UTC timestamp; recheck after changing settings. Unapplied codec
edits remain untouched and are not included in the saved diagnostic snapshot.

## Live quality

PN mode samples the running driver's status, packet counters and optional quality
telemetry about once per second. The page shows current bitrate and the negotiated,
configured ceiling, packet progress, drops, write failures and latency trims in an
observation window of up to 60 seconds. The graph shows bitrate over time; orange
marks a bitrate reduction or an increase in transport errors.

Idle, sampling, healthy transport, congestion, recovery and stalled packet progress
have separate descriptions. Recovery uses the driver's current clean-traffic
requirement (30/10/5 seconds, or 60 after a quick relapse, in driver 0.1.0.18).
Older drivers retain the 30-second display. This is not a promise of a bitrate
increase at a particular wall-clock time. Fixed-rate codecs cannot be lowered in-band.

The window resets on failed samples, counter resets, codec/connection-state or
audio-active transitions and gaps longer than five seconds. Samples older than
2.5 seconds are stale. Idle time never establishes healthy playback. The window is
bounded in memory and clears when the selected device/session changes.

Windows mode does not expose Bluetooth codec, bitrate or packet-health telemetry
through these interfaces. The Quality page says unavailable and still offers the
standard device/output checks. Demo mode is visibly marked and does not query the
physical receiver through diagnostics.

## Why quality changed

**Quality > Events** shows the last 32 observed stream/codec/bitrate changes.
Driver 0.1.0.18 supplies the latest adaptation reason: slow writes, failed writes,
queue pressure, audio backlog or recovery. Unobserved changes and older drivers
show an unavailable cause. Events survive observation interruptions for the same
receiver; selecting another receiver clears them. This is an observation history,
not a complete driver trace. The dialog is a snapshot; reopen it to refresh.

A codec rate that differs from the Windows rate (driver 0.1.0.22 and later) is not a fault: the
driver converts it with at most 2.07 ms of added latency (part of the reported latency)
and no measurable loss (THD+N below -139 dB from 24-bit sources, no alias or image above
-122 dB). The status shows both rates. It happens when the receiver lacks the Windows rate, or
when Windows changed its rate after the stream had played; a stream that has not played
yet is moved to the Windows rate before the sound starts (0.1.0.22 review). A conversion it cannot do (from 32 or 16 kHz, from 176.4 / 192 kHz, or up to
88.2 / 96 kHz) still selects another codec or renegotiates, as before.

The format line then names both rates, for example **"44100 Hz (Windows 48000 Hz,
converted), 2 ch, 32-bit in"**: the receiver still runs at 44.1 kHz and the driver converts
Windows' 48 kHz (seen on the EDIFIER MR5 with driver 0.1.0.23 after Windows was switched to
48 kHz during playback). Since driver 0.1.0.23 the Windows rate is the format chosen under
**Sound > device > Properties > Advanced > Default Format**; earlier PN drivers made Windows'
shared-mode engine use the driver's first listed format (normally 48 kHz) whatever was chosen. If the Windows rate shown is not the one you chose, the
playing application has not reopened its stream yet (Chromium-based browsers keep it open
while paused): close and reopen it.

With the Music latency profile (driver 0.1.0.22), a short stall can show a growing peak
audio backlog (`pna2dpctl quality`) without a congestion reason or a bitrate change: the
buffer absorbs it, and a backlog the link is draining is not counted. Trims
(`pna2dpctl stats`, latency trims) and reductions then appear only when a stall outlasts
the 450 ms maximum or the link stays slower than the stream. `pna2dpctl config` shows the
active profile.

After at least ten seconds of persistent trouble on aptX, aptX HD or aptX LL,
**Use adaptive SBC** can become available. It requires current observations,
receiver/encoder SBC support and no unsaved edits. Clicking it saves SBC first in
the codec preference list and enables ABR using the saved SBC settings; playback
may briefly interrupt. It never silently changes codecs, and a 60-second cooldown
prevents repeated requests. Settings on other codec pages remain saved.

## Status flags, counters and state names (driver 0.1.0.19 and later)

The driver reports these additions to ABI 1 (`pna2dp_public.h`); older drivers leave
them 0.

| Field | Meaning | Where it appears |
|---|---|---|
| `PNA2DP_STATUS_FLAG_REMOTE_SUSPENDED` (0x200) | The receiver suspended the stream; the driver waits instead of restarting it at once | Status page flags: "suspended by receiver" |
| `PNA2DP_STATUS_FLAG_UNREACHABLE` (0x400) | Link lost or media stalled without an answer; the driver is retrying | Status page flags: "receiver unreachable" |
| `PNA2DP_STATUS_FLAG_HALF_RATE` (0x800) | The engine runs at twice the codec rate and the 2:1 stage alone converts it | Status page flags: "rate converted (2:1)" ("2:1 rate conversion" from a 0.1.0.21 driver) |
| `PNA2DP_STATUS_FLAG_RATE_CONVERTED` (0x1000, driver 0.1.0.22) | The stream runs through the sample-rate converter: the Windows rate differs from the codec rate | Status page flags: "rate converted"; Windows rate row "44100 Hz, converted to 48000 Hz"; `pna2dpctl status` format line "48000 Hz (Windows 44100 Hz, converted), ..." |
| `PNA2DP_STATUS.EngineSampleRate` (driver 0.1.0.22, former reserved word) | The Windows audio engine's rate while a stream is open, else 0 | Status page format line, `pna2dpctl status`, diagnostic export ("Windows rate") |
| `PNA2DP_STATS.WaveStarvedPackets` | Packets the Windows engine had not written in time; sent as faded silence | `pna2dpctl stats`: "starved packets" |
| `PNA2DP_STATS.RingOverflowEvents` | PCM ring overruns: the oldest audio was discarded (events, not bytes) | `pna2dpctl stats`: "overflows" |
| `PNA2DP_STATS.FragmentedPackets` | AAC access units that needed more than one RTP packet | `pna2dpctl stats`: "AAC fragmented sends" |
| `PNA2DP_STATS.HostDroppedBlocks` | Encode blocks lost at the codec host (invalid or unanswered results) | `pna2dpctl stats`: "dropped blocks" |

`PNA2DP_STATS.Underruns` keeps its historical name but counts latency-guard trims
(shown as "latency trims"); WaveRT starvation is `WaveStarvedPackets`.

`SinkReportedDelayUs` is the receiver's AVDTP delay report for the current stream and is
0 when none was received. Drivers 0.1.0.20 to 0.1.0.22 also left it 0, and ignored the
report, when delay reporting had not been negotiated; driver 0.1.0.23 uses a report
whenever the receiver sends one (the EDIFIER MR5 sends about 364..396 ms on its LDAC stream
without offering the capability). `EstimatedLatencyUs` adds the report, or a per-codec
default sink delay when there is none.

| Status page sink delay | `pna2dpctl status` | Meaning |
|---|---|---|
| "N ms reported by the headset" | "sink reports N ms" | The receiver reported its delay, with delay reporting negotiated |
| "N ms reported by the headset" | "sink reports N ms (reported without delay-reporting negotiation)" | The receiver reported its delay although delay reporting was not negotiated; it is used (driver 0.1.0.23) |
| "not reported; a per-codec default is assumed" | "sink reports 0.0 ms", plus "(not reported; a per-codec default is assumed)" when delay reporting was not negotiated | No report: the estimate uses the codec's default (SBC and aptX 150 ms, AAC and aptX HD 180 ms, LDAC 200 ms, aptX LL 60 ms) |

In application 0.1.0.34 the driver state `PNA2DP_STATE_DISCOVERING` is shown as
**Connected** in the panel, `pna2dpctl`, quality events and the diagnostic export. The
driver reports it whenever signalling is up without a configured stream, which is mostly
a connected, idle receiver; discovery itself takes well under a second.

## Diagnostic export

After a completed check, **Export diagnostics** saves a local UTF-8 `.txt` report.
It contains the check snapshot/time, app and driver versions, platform state,
codec/receiver capabilities, saved preferences, lifetime error counters and the
recent numeric timeline and observed quality events. Lifetime counters can include earlier incidents; the
timeline is observed later than the saved check and uses ages relative to export.

The report uses an explicit field allowlist. Device names, Bluetooth addresses,
endpoint/instance IDs, installation/user paths and free-form raw driver logs are
omitted. No PCM, encoded audio, recording or network upload is performed. File
replacement happens only after a complete UTF-8 report has been written/flushed;
failed replacement preserves an existing file. Text exports currently use the
same MAX_PATH limit as settings export.

## Verification

`build/build.ps1 -Target health-tests` builds `health_test.exe`. Its default mode
exercises the production diagnostic model and export helper with fixtures for
missing devices/outputs, query failures, mute, encoder/service/capability issues,
Windows mode, privacy, stalled streams, counter resets, gaps and recovery. It is
also included in normal verification and AddressSanitizer runs.

For a read-only integration check of one exact PnP device instance:

```powershell
.\out\Release\health_test.exe --live '<device instance ID>' out\health-report.txt
```

The live check reads configuration and writes only the requested report. It does
not play audio or measure a historical window by itself. Use the live panel while
audio plays for the rolling transport observations. Neither a clean transport
window nor a successful endpoint query proves audible output, subjective sound
quality, RF signal strength, available bandwidth or support for untested receivers.
