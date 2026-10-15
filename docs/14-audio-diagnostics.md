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
| `PNA2DP_STATUS.SinkDelayUs` (driver 0.1.0.25, former reserved word) | The speaker delay the driver applies (estimate and presentation position): the fixed setting, a plausible report or the per-codec default; 0 without a stream | Status page **Sink delay**, Headset page speaker-delay line, `pna2dpctl status` **sink delay** line |
| `PNA2DP_STATUS_FLAG_SINK_DELAY_SET` (0x2000, driver 0.1.0.25) | `SinkDelayUs` is the fixed speaker delay (`PNA2DP_CONFIG.SinkDelayMs`) | Sink delay texts: "N ms fixed" |
| `PNA2DP_STATUS_FLAG_SINK_DELAY_IGNORED` (0x4000, driver 0.1.0.25) | The receiver's report is below its codec's plausibility floor and is not used (also set while a fixed delay applies) | Sink delay texts: "the N ms report is ignored" |
| `PNA2DP_STATS.WaveStarvedPackets` | Packets the Windows engine had not written in time; sent as faded silence | `pna2dpctl stats`: "starved packets" |
| `PNA2DP_STATS.RingOverflowEvents` | PCM ring overruns: the oldest audio was discarded (events, not bytes) | `pna2dpctl stats`: "overflows" |
| `PNA2DP_STATS.FragmentedPackets` | AAC access units that needed more than one RTP packet | `pna2dpctl stats`: "AAC fragmented sends" |
| `PNA2DP_STATS.HostDroppedBlocks` | Encode blocks lost at the codec host (invalid or unanswered results) | `pna2dpctl stats`: "dropped blocks" |

`PNA2DP_STATS.Underruns` keeps its historical name but counts latency-guard trims
(shown as "latency trims"); WaveRT starvation is `WaveStarvedPackets`.

`SinkReportedDelayUs` is the receiver's AVDTP delay report for the current stream and is
0 when none was received. Drivers 0.1.0.20 to 0.1.0.22 also left it 0, and ignored the
report, when delay reporting had not been negotiated; driver 0.1.0.23 uses a report
whenever the receiver sends one (the EDIFIER MR5 sends one on its LDAC stream without
offering the capability). Since driver 0.1.0.25 `SinkReportedDelayUs` holds the report
whether or not it is used, and `SinkDelayUs` holds the delay actually applied, in this
order: a fixed speaker delay set for this codec or any codec; else the report, when it is
at or above the codec's plausibility floor (LDAC, AAC and aptX HD 80 ms; aptX, SBC and
other codecs 40 ms; aptX LL none); else the codec's default (SBC and aptX 150 ms, AAC and
aptX HD 180 ms, LDAC 200 ms, aptX LL 60 ms). `EstimatedLatencyUs` is the pipeline plus
`SinkDelayUs`. The MR5 reports 25.0 ms right after it sets up the stream itself (ignored:
below the LDAC floor) and 364.0 ms later (used), while a microphone measured its real
delay at 267-340 ms; see [speaker delay and lip-sync](21-speaker-delay-and-lip-sync.md).

| Status page sink delay (driver 0.1.0.25) | `pna2dpctl status`, **sink delay** line | Meaning |
|---|---|---|
| "364 ms reported by the speaker" | "Using 364 ms reported by the speaker" | A plausible report is used |
| "315 ms fixed; the speaker reports 364 ms" | "Using 315 ms, the fixed speaker delay; the speaker reports 364 ms" | The fixed speaker delay applies; the report is shown for comparison |
| "315 ms fixed; the 25 ms report is ignored" | "Using 315 ms, the fixed speaker delay; the speaker's report of 25 ms is ignored as impossible" | Fixed delay; the report is below the floor |
| "200 ms default; the 25 ms report is ignored" | "Using 200 ms, the LDAC default; the speaker's report of 25 ms is ignored as impossible" | The report is below the floor; the codec's default applies |
| "150 ms default; not reported" | "Using 150 ms, the SBC default; the speaker does not report its delay" | No report: the codec's default |
| "no stream" | "No stream" | No AVDTP stream: nothing applies |

`pna2dpctl status` prints the end-to-end estimate on its own **latency** line ("N ms
estimated end to end") and adds "(reported without delay-reporting negotiation)" to the
**sink delay** line when the receiver reported without negotiating it. With a driver before
0.1.0.25 (`SinkDelayUs` stays 0), the panel and `pna2dpctl` show the former texts: "N ms
reported by the headset" or "not reported; a per-codec default is assumed".

The Status page's **Flags** row does not list the two new flags; the **Sink delay** row
says the same in words. The driver's trace (`pna2dpctl trace`, elevated) logs `sink delay
report 25.0 ms ignored: below the 80 ms floor of LDAC`, `speaker delay set: 315 ms for LDAC`
or `speaker delay: automatic`, and, for a configuration the receiver chose,
`remote configured LDAC @ 48000 Hz, 990000 bps (encoder N)` (driver 0.1.0.24 and later
apply the saved quality to such a configuration).

In application 0.1.0.34 the driver state `PNA2DP_STATE_DISCOVERING` is shown as
**Connected** in the panel, `pna2dpctl`, quality events and the diagnostic export. The
driver reports it whenever signalling is up without a configured stream, which is mostly
a connected, idle receiver; discovery itself takes well under a second.

## Speaker delay measurement

**Headset > Speaker delay (lip-sync) > Measure with microphone** and `pna2dpctl
measure-delay` measure how long the speaker really takes. Unlike **Check my audio**, they
play sound, and only on an explicit request: 8 short chirps (12 ms sweeps from 1 to
7 kHz) over about 13 seconds on the speaker's own Windows output, recorded by the chosen
microphone. The recording is analysed in memory and not saved; nothing of it reaches a
file, the diagnostic export or the network. The result is the delay the driver applied
during the run plus how late the sound came, and it becomes a pending **Fixed** value
(panel) or is printed with the command that saves it (`pna2dpctl`; `apply` saves it).

`pna2dpctl measure-delay [-d N] [mic=<name part>] [apply]` prints:

```
speaker    : <the speaker's Windows output>
microphone : <the microphone>
Playing 8 short chirps over about 13 s. ...
  Starting the speaker...
  Waiting for the speaker to start playing...
  Chirp 1 of 8...                      (one line per chirp)
  Listening for the last chirp...
  Analysing the recording...

chirp   late by      match              (one row per chirp: lateness and match 0..1)
median <ms>, spread <ms>, <n> of 8 clear
<n> of 8 chirps heard clearly, spread <ms>: the sound comes <ms> later (or earlier) than the output's clock says.
microphone input: <rate> Hz, <channels> ch, <bits>-bit[ float][, raw (no effects)]
speaker delay: <ms> ms for <codec> (the driver applied <ms>; the sound came <ms> later or earlier)
not saved; run measure-delay with apply, or: pna2dpctl set sinkdelay=<ms> sinkdelay.codec=<codec>
```

A chirp row can also read "not at the run's delay: heard at ..." (heard at another delay)
or "not heard". The command exits with 1 and a plain-language reason when the run gives no
value: no microphone or several matching the name part (it lists them), a microphone that
records below 16 kHz, only silence recorded, fewer than 6 of 8 chirps heard (raise the
volume or move the microphone closer), chirps that disagree by more than 20 ms or two chirps
heard at another delay (noise, or the speaker changed its delay), the speaker's clock not
starting or stopping, the driver's applied delay or codec changing during the run, the
stream restarting, a result outside 1..1000 ms, or a driver before 0.1.0.25. Ctrl+C
cancels. The panel shows the same reasons below the button.

The MR5's delay changes by about +-30 ms from one stream start to the next, so a single
measurement can be off by tens of milliseconds: measure three to five times from silence
and keep the middle value.

## Diagnostic export

After a completed check, **Export diagnostics** saves a local UTF-8 `.txt` report.
It contains the check snapshot/time, app and driver versions, platform state,
codec/receiver capabilities, saved preferences, lifetime error counters and the
recent numeric timeline and observed quality events. Lifetime counters can include earlier incidents; the
timeline is observed later than the saved check and uses ages relative to export.

The report uses an explicit field allowlist. Device names, Bluetooth addresses,
endpoint/instance IDs, installation/user paths and free-form raw driver logs are
omitted. No PCM, encoded audio, recording or network upload is performed; a speaker-delay
measurement's microphone recording never reaches the report. The status flags appear in
hexadecimal (0x2000 / 0x4000: a fixed speaker delay applies / a report was ignored); the
delay values and the speaker-delay setting are not part of the report. File
replacement happens only after a complete UTF-8 report has been written/flushed;
failed replacement preserves an existing file. Text exports currently use the
same MAX_PATH limit as settings export.

## Verification

`build/build.ps1 -Target health-tests` builds `health_test.exe`. Its default mode
exercises the production diagnostic model and export helper with fixtures for
missing devices/outputs, query failures, mute, encoder/service/capability issues,
Windows mode, privacy, stalled streams, counter resets, gaps and recovery. It is
also included in normal verification and AddressSanitizer runs.

`build/build.ps1 -Target latency-tests` builds `latency_test.exe` (151 checks for
v0.1.0.38): the speaker-delay measurement's analysis on synthetic recordings (known delays,
noise, missing chirps, reflections, clock and timestamp jitter, alias candidates, several
microphone channels, a delay change during the run, low-rate microphones) and the
conversion of a run into a speaker delay. It plays and records nothing. `verify.ps1` runs
it, also under AddressSanitizer. `pnctl_test` checks the sink-delay texts above.

For a read-only integration check of one exact PnP device instance:

```powershell
.\out\Release\health_test.exe --live '<device instance ID>' out\health-report.txt
```

The live check reads configuration and writes only the requested report. It does
not play audio or measure a historical window by itself. Use the live panel while
audio plays for the rolling transport observations. Neither a clean transport
window nor a successful endpoint query proves audible output, subjective sound
quality, RF signal strength, available bandwidth or support for untested receivers.
