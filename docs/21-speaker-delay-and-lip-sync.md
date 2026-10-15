# Speaker delay and lip-sync: driver 0.1.0.25 / application 0.1.0.38

Record for the [v0.1.0.38 prerelease](releases/v0.1.0.38.md): application, control panel
and installer **0.1.0.38**, custom driver **0.1.0.25** (`DriverVer = 10/15/2026,0.1.0.25`).
Two driver changes and one application change:

1. **Driver 0.1.0.24:** a configuration the **receiver** chooses (its AVDTP
   SET_CONFIGURATION or RECONFIGURE) takes the source-side choices from the saved
   preferences, as a configuration PN-A2DP chooses always did. Before, a speaker that
   connected by itself ran LDAC at SQ 660 kbps under an HQ preference.
2. **Driver 0.1.0.25:** the speaker delay. A delay report too low to be real is ignored, a
   per-device speaker delay can replace the reports, and the status says which delay is
   applied and where it comes from.
3. **Application 0.1.0.37 / 0.1.0.38:** a microphone measurement of the speaker delay (panel
   and `pna2dpctl measure-delay`), the speaker-delay settings in the panel and the command
   line, and one decoder of a receiver's sample rates for every codec.

Driver 0.1.0.24 and application 0.1.0.37 were intermediate builds, installed on the test PC
only and never published; everything they contain ships in 0.1.0.25 / 0.1.0.38.

Status on 15 October 2026: **software-verified, with partial hardware validation** on one
receiver (EDIFIER MR5, LDAC) and one microphone. See [hardware results](#hardware-results-15-october-2026),
[how to measure well](#how-to-measure-well), [verification](#verification) and
[limits and checks still to do](#limits-and-checks-still-to-do).

## Summary

| Area | 0.1.0.23 / 0.1.0.36 | 0.1.0.25 / 0.1.0.38 |
|---|---|---|
| Receiver-chosen configuration | the parser's defaults: LDAC SQ, AAC with the receiver's VBR bit and bitrate, SBC from the top of the receiver's bitpool range, aptX LL level 0 | the saved preferences: LDAC quality and input format, AAC bitrate and CBR / VBR (written into the IEs the AAC plugin reads), SBC start bitpool and floor within the receiver's range, aptX LL target level, the basic-rate budget |
| Delay report | used whenever the receiver sends one | used when at or above the codec's plausibility floor; below it the per-codec default applies and the status flags the report as ignored |
| Speaker delay setting | none | **Automatic** (default) or **Fixed** 1..1000 ms, for one codec or any codec; applies live, without renegotiation |
| Status | `SinkReportedDelayUs` (the report), `EstimatedLatencyUs` | also `SinkDelayUs` (the delay applied), `SINK_DELAY_SET` (0x2000), `SINK_DELAY_IGNORED` (0x4000) |
| Measurement | none | **Measure with microphone** (Headset page) and `pna2dpctl measure-delay`: 8 chirps, about 13 s, analysed in memory |
| Receiver sample rates | decoded per codec in several places; `pna2dpctl caps` showed rates for SBC only | `pn::SepRatesText` for SBC, AAC, the aptX family, LDAC, FastStream and Opus; `pna2dpctl caps` prints them for every codec |

## Why: the MR5's reports against a microphone

A video player keeps picture and sound together with the presentation position Windows gives
it (`IAudioClock::GetPosition`): the frame the listener hears now. For a Bluetooth endpoint
the driver computes that position from the audio still in its pipeline plus the receiver's
delay. Since driver 0.1.0.23 the receiver's AVDTP delay report is used whenever one arrives,
even without delay-reporting negotiation ([maximum audio quality](20-max-audio-quality.md#sink-delay-reports-were-ignored-without-negotiation)).

The EDIFIER MR5 (LDAC, 48 kHz) reports **25.0 ms** right after it sets up the stream itself
and **364.0 ms** later, after a stream restart. Short chirps played on the speaker and
recorded with a microphone, timed against the render stream's `IAudioClock` (which already
includes the delay the driver applied), showed that neither is its delay:

| MR5 report applied by the driver | Sound against the clock | Speaker delay (applied + lateness) |
|---|---|---|
| 25.0 ms | **267.6 ms late** (8 chirps, within 3.5 ms) | about 293 ms |
| 364.0 ms | **86-99 ms early** | about 265-278 ms |

Over 10 normal stream starts the MR5's real delay was **267-340 ms**, median about
**315 ms** ([below](#ten-stream-starts)). So a player either showed the picture about 270 ms
before the sound (25 ms report) or 24-97 ms after it (364 ms report). The 25 ms report is
impossible for an LDAC sink, which decodes and buffers far longer; 364 ms is plausible but
wrong for this speaker. Two changes follow: a plausibility floor catches the impossible
reports, and a measured fixed delay fixes the rest.

## Driver 0.1.0.24: receiver-chosen configurations follow the saved preferences

A SET_CONFIGURATION or RECONFIGURE from the receiver fixes the sample rate, the channel
mode and, for SBC, the bitpool range. The rest is the source's own choice, and the driver
used to leave it at what `A2dpParseConfig` fills in: LDAC at SQ, so the MR5, reconnecting by
itself after a settings change, ran LDAC at **660 kbps (ceiling 660, SQ)** under an HQ
preference. `A2dpApplySourcePreferences` (`a2dp_codec.cpp`) now applies the same preference
rules as `A2dpSelectConfig`; `OnRemoteSetConfiguration` and `OnRemoteReconfigure`
(`device.cpp`) call it with a copy of the device's configuration taken under `StatusLock`
and the radio's basic-rate state (`ConservativeRadio`).

- **LDAC:** the saved quality (HQ, SQ or MQ; adaptive starts at SQ, as the chooser does) and
  the saved input format (32-bit, else 24-bit, else float when the mask allows it).
- **AAC:** VBR only when both the receiver and the preference allow it; the bitrate is the
  smaller of the receiver's and the saved one (the saved one when the receiver states none,
  256 kbps when neither does), at most 320 kbps, so it never exceeds the receiver's. The codec host's AAC plugin takes the encoder mode and
  peak bitrate from the IEs, so the VBR bit and the bitrate are written into IE bytes 3..5;
  every other IE byte stays the receiver's.
- **SBC:** the receiver's range (minimum..maximum bitpool) is binding. A saved maximum below
  the receiver's sets where the encoder starts (`SbcBitpool`, the ABR's top rung), and the
  saved minimum sets the floor, both within the range; `SbcMaxBitpool` stays the
  receiver's maximum, which a live bitpool change may still reach in band.
- **aptX, aptX HD, aptX LL:** the saved target buffer level of that codec.
- **Basic Rate-only radio:** the 350 kbps starting budget (`A2dpConstrainBasicRadio`: LDAC
  MQ, a lower SBC bitpool), as for a stream PN-A2DP configures; the receiver's IEs and SBC
  maximum stay as configured.
- The nominal bitrate becomes both the current bitrate and the ceiling. The trace line
  reads `remote configured LDAC @ 48000 Hz, 990000 bps (encoder N)`.

The receiver's rate and mode stand, and so do its IEs apart from AAC's mode and bitrate
bytes, so nothing is renegotiated by this change. The
reconfigure-before-first-START rules of driver 0.1.0.22 (the engine rate, the SBC channel
mode preference) are unchanged.

## Driver 0.1.0.25: the speaker delay

### Plausibility floor

No receiver presents audio sooner than it decodes a frame and fills its jitter buffer, so a
report below a floor is not its delay (`transport_policy.h` `PnMinPlausibleSinkDelayUs`):

| Codec | Floor | Default used instead (unchanged) |
|---|---|---|
| LDAC | 80 ms | 200 ms |
| AAC | 80 ms | 180 ms |
| aptX HD | 80 ms | 180 ms |
| aptX | 40 ms | 150 ms |
| SBC and unknown codecs | 40 ms | 150 ms |
| aptX Low Latency | none: any report is believed | 60 ms |

A report at the floor is used; below it the per-codec default applies, the status sets
`PNA2DP_STATUS_FLAG_SINK_DELAY_IGNORED`, and the trace reads `sink delay report 25.0 ms
ignored: below the 80 ms floor of LDAC`. `core_test` asserts that every default lies above
its codec's floor.

### The setting

`PNA2DP_CONFIG.SinkDelayMs` (0 = automatic, else 1..1000 ms) and `SinkDelayCodec` (a
`PNA2DP_CODEC_*`, 0 = any codec), stored per device with the rest of the configuration.
`PnConfigValidate` clamps a value above 1000 ms to 1000 and turns a codec word that is no
codec into automatic. `PnSinkDelayDecide` picks the delay for the current stream:

1. no AVDTP stream: 0;
2. a fixed delay set for any codec or for this stream's codec: that value;
3. a report of this stream at or above the floor: the report;
4. otherwise the per-codec default.

A fixed delay for LDAC leaves the speaker's other codecs automatic. One value is stored per
device, so a speaker used with two codecs gets a fixed delay for one of them (or the same
value for any codec).

A changed setting applies at once: `DeviceSetConfig` treats it like a live bitpool change
and renegotiates nothing for it, and traces `speaker delay set: 315 ms for LDAC` or
`speaker delay: automatic`. The presentation position reads the setting lock-free at up to
DISPATCH level, so both words travel as one interlocked `LONG` (milliseconds in the low 16
bits, the codec above them; `PnSinkDelaySettingPack`), published at `DeviceStart` and with
every `DeviceSetConfig` under `StatusLock`.

### Status

- `SinkReportedDelayUs` keeps its ABI meaning: the receiver's last report for this stream,
  0 if none, now whether or not it is used.
- `SinkDelayUs` (new): the delay the driver applies, 0 without a stream.
- `EstimatedLatencyUs` = pipeline + `SinkDelayUs`.
- `PNA2DP_STATUS_FLAG_SINK_DELAY_SET` (0x2000): `SinkDelayUs` is the fixed setting.
- `PNA2DP_STATUS_FLAG_SINK_DELAY_IGNORED` (0x4000): the report is below the floor; also set
  while a fixed delay applies, so the tools can say so.

### Presentation position

The WaveRT presentation position subtracts the pipeline and the applied sink delay, as the
status estimate adds them. It already followed latency changes at no more than 1 % of real
time (10 ms per second) and never moved backwards; a changed speaker delay goes through the
same slew, so moving from a 364 ms report to a fixed 315 ms settles in about 5 seconds of
playback without a jump. `core_test` simulates 20 seconds of a running stream with the
delay changed live at 5 s (364 to 280 ms, the LDAC default 200 to 280 ms, 280 back to
364 ms): no backward step, every 10 ms step within the slew, and within 1 ms of the new
delay at the end.

The speaker delay changes only what Windows tells players about the delay. It adds no
buffering and does not delay the sound itself; a player that syncs picture to the position
moves the picture.

### ABI

ABI 1 is unchanged. `PNA2DP_CONFIG.Reserved[0..1]` became `SinkDelayMs` / `SinkDelayCodec`
(156 bytes, offsets 124 / 128) and `PNA2DP_STATUS.Reserved[0]` became `SinkDelayUs` (224
bytes, offset 212); the flags are free bits. A tool that sends zeros there selects automatic;
a driver before 0.1.0.25 reports `SinkDelayUs` 0, and the tools then show the former texts.
The panel disables the speaker-delay group and `pna2dpctl measure-delay` refuses to run
(`pn::DriverAppliesSinkDelay`). `pna2dpctl set sinkdelay=` is not version-checked: an older
driver accepts the words and ignores them. `core_test` asserts the layout.

## The measurement engine (`tools/common/pnlatency`)

Shared by the panel and `pna2dpctl`. The pure analysis (`AnalyzeLatency`) is kept apart from
the WASAPI part (`MeasureOutputLatency`), so `latency_test` covers the analysis without any
device.

### Signal and schedule

- The chirp: a 12 ms linear sweep from 1 to 7 kHz, Hann window, at half full scale on every
  channel of the speaker's Windows output.
- 8 chirps: 3 s of silence first (the Bluetooth stream starts and the speaker fades in),
  then gaps of 900, 1150, 1000, 1300, 950, 1200 and 1050 ms, then a 2 s tail: about
  13 seconds.
- The gaps are deliberately uneven. With uniform spacing a lag wrong by one period lines up
  seven of eight chirps with a neighbour; with these gaps a wrong lag lines up at most one,
  so the single lag that fits all chirps is unambiguous. Every gap differs from every other
  by at least 40 ms (four times the timing tolerance), and any two consecutive gaps together
  are longer than the 2000 ms lag search span.

### Recording

- The output: the Windows endpoint of the selected PN device (never the default output), in
  shared mode at its 32-bit float mix format, with 100 ms kept queued. Its audio clock
  (`IAudioClock::GetPosition`, position and QPC time) is sampled about every 5 ms. In PN
  driver mode that position is the driver's presentation position, which already subtracts
  the pipeline and the applied speaker delay.
- The microphone: shared mode, in **raw mode** (no Windows effects in the timing path) where
  the endpoint allows it, every channel up to 8. Each capture packet's QPC timestamp times
  its samples, so microphone and clock share one time base. A microphone that records below
  16 kHz is refused before anything plays: the chirp reaches 7 kHz and would fold over
  (Bluetooth headset microphones often record at 8 kHz).
- The run ends when the clock has presented every frame; a clock that does not move for 8 s
  stops it. Cancel stops both streams within about 20 ms.
- **The recording is analysed in memory and not saved**: no file, no export, no upload.

### Analysis

- Each chirp's presentation time comes from the clock sample nearest its first frame (none
  within 1 s: the chirp is untimed).
- A matched filter gives the normalized correlation of the chirp at every microphone
  position (0..1); a chirp is **clear** at 0.5 or more.
- **One global lag:** for every lag from -500 to +1500 ms in 1 ms steps, each chirp's best
  match within 10 ms is summed over the chirps; the highest sum is the run's lag, centred on
  the median of the clear chirps near it. Each chirp is then located within that lag
  +-20 ms, and also within +-400 ms to see a chirp heard at another delay.
- **Verdict:** a result needs at least 6 of 8 clear chirps, a spread (largest minus smallest
  lateness) of at most 20 ms, and no two other chirps heard clearly together at another delay
  (the speaker changed its delay during the run). The lateness is the median of the clear
  chirps. Otherwise the run is *not heard* (fewer than 6 chirps heard: volume or distance,
  or a silent microphone), *inconsistent* (noise, or a delay change; the message names the
  difference), or it failed (no clock, no timed recording, a Windows audio error,
  cancelled), each with a plain-language message.
- A microphone with several channels (an interface's second input, a stereo microphone) is
  analysed per channel; the best channel wins (a result first, then the most clear chirps,
  then the highest summed match), and a channel equal to an earlier one is skipped.

### From lateness to a speaker delay

The clock already subtracts the delay the driver applies, so the speaker's own delay is that
applied delay plus the lateness (`SpeakerDelayFromRun`). The caller reads `SinkDelayUs` and
the stream's codec from the driver when the clock starts, at every chirp and once after the
last chirp, while the stream still runs. The result is refused, with a message, when the
driver does not report the value (before 0.1.0.25), when a reading is 0 (the stream started
or restarted during the run), when the codec or the value changed during the run, when no
reading was taken after the last chirp, or when the result is outside 1..1000 ms.

A driver that applies far more delay than the speaker has presents each chirp before it
sounds (a fixed 1000 ms on a 280 ms speaker: 720 ms early). Above 480 ms of expected applied
delay the lag search starts at -(delay + 30 ms), the delay counted up to 1000 ms, and keeps
its 2000 ms span (`FitLatencyLagRange`; the expected delay is the larger of the status value
and a fixed setting that applies, `pn::ExpectedSinkDelayMs`).

## Control panel

**Headset** page, new group **Speaker delay (lip-sync)** (the receiver list and detail above
it are shorter to make room):

- A status line: "Using 364 ms reported by the speaker", "Using 315 ms, the fixed speaker
  delay; the speaker reports 364 ms", "Using 200 ms, the LDAC default; the speaker's report of
  25 ms is ignored as impossible", "Using 150 ms, the SBC default; the speaker does not report
  its delay", or "No stream". With a driver before 0.1.0.25: "Speaker delay settings and the
  measurement need driver 0.1.0.25 or later.", and the group is disabled.
- **Automatic** / **Fixed:** with a millisecond box (1..1000) and its scope, for example
  "ms, for LDAC" or "ms, for any codec". A fixed value applies to the codec playing when
  Fixed is chosen or the value is typed (kept while nothing plays); an empty box starts from
  the delay in use (150 ms without a stream).
- **Microphone:** the active recording devices, Windows' default marked "(default)" and
  selected first; the list is read again when it is opened and on **Refresh**.
- **Measure with microphone** asks first: which speaker and microphone, 8 short chirps over
  about 13 seconds, "The recording is analysed in memory and not saved", the microphone
  10 to 30 cm from the speaker, other playback stopped, a quiet room, a normal listening
  volume. When Windows is already playing to the speaker it adds that the test needs a fresh
  stream. While it runs the button reads **Cancel measurement** and the text below follows
  the run: "Starting the speaker...", "Waiting for the speaker to start playing...",
  "Chirp 3 of 8...", "Listening for the last chirp...", "Analysing the recording...".
- The result, for example "Measured 306 ms (8 of 8 chirps, spread 10 ms). Apply changes to
  use it.", goes in as a pending **Fixed** value for the codec that played. **Apply changes**
  saves it (the text adds "Applied."); **Undo changes** drops it ("Not applied."). A failed
  run shows its message and changes nothing.
- The measurement runs on a worker thread with its own read-only driver handle; closing the
  panel or changing the device cancels it. Demo mode simulates the progress and a result
  without sound or devices.

**Status** page, **Sink delay** row: the applied delay and its source, "364 ms reported by the
speaker", "315 ms fixed; the speaker reports 364 ms", "200 ms default; the 25 ms report is
ignored", "150 ms default; not reported", "no stream" (drivers before 0.1.0.25: the former
"reported by the headset" texts).

Presets and **Auto quality + codecs** keep the speaker delay, like the volume: it belongs
to the speaker. The footer's **Defaults** button loads the driver defaults, so the speaker
delay returns to **Automatic** (the volume is kept, as before).

`build/check-panel-text-fit.ps1` knows the longest text of every new label: the Status page's
sink-delay texts, the Headset page's status line, the scope label and the two-line
measurement text (the hint, the longest failure messages and the result; the progress texts
are shorter).

## Command line

```
pna2dpctl set -d N sinkdelay=315 sinkdelay.codec=ldac   a fixed 315 ms for LDAC
pna2dpctl set -d N sinkdelay=auto                       automatic again (also clears the codec)
pna2dpctl config -d N                                   "sink delay     : 315 ms (LDAC)" or "automatic"
pna2dpctl status -d N                                   "sink delay  : Using 315 ms, the fixed speaker delay; ..."
pna2dpctl measure-delay [-d N] [mic=<name part>] [apply]
```

- `sinkdelay=auto|0|1..1000` (milliseconds) and `sinkdelay.codec=any|sbc|aac|aptx|aptxhd|aptxll|ldac`
  (case-insensitive); other values are refused with a message and change nothing. A new
  value keeps the codec already set.
- `pna2dpctl status` prints the end-to-end estimate on its own line and the speaker delay
  with its source on a **sink delay** line, adding "(reported without delay-reporting
  negotiation)" where that applies.
- Settings export writes `sinkdelay=auto|N` and `sinkdelay.codec=any|<codec>`; on import an
  edited line wins over the blob, and an invalid value is ignored.
- `measure-delay` uses the default microphone, or the one whose name contains the given part
  (several matches or none: it lists the microphones). It prints the speaker and microphone,
  the progress, a per-chirp table, the microphone format, and
  `speaker delay: N ms for LDAC (the driver applied X ms; the sound came Y ms later)`. Without
  `apply` it prints the `pna2dpctl set -d <device> sinkdelay=<ms> sinkdelay.codec=<codec>`
  command for the device it measured (the first build left out `-d`, which would have saved
  the value for device 0; fixed before the release). With `apply` it saves
  the value for the codec that played. Ctrl+C cancels. It needs driver 0.1.0.25 or later.
- `pna2dpctl caps` prints a `rates` line for every codec (for example
  `rates 44.1k, 48k, 96k` for LDAC); SBC keeps its existing line.

## Receiver sample rates (application 0.1.0.37)

`pn::SepRatesText` decodes a remote endpoint's sample rates for SBC, AAC (8 to 96 kHz), the
aptX family, LDAC (44.1 / 48 / 88.2 / 96 kHz), FastStream (the music direction) and Opus
(48 kHz), or "-" for a short or unknown element. The Headset page's endpoint detail and
`pna2dpctl caps` both use it. After the owner enabled the MR5's own 96 kHz setting, its LDAC
endpoint advertises 44.1, 48 and 96 kHz (IE ending `34 07`; before `30 07`), and
`pna2dpctl caps` shows "rates 44.1k, 48k, 96k".

The stream follows the Windows format: at a 48 kHz Default Format the MR5 still runs LDAC at
48 kHz. LDAC at 96 kHz needs a 96 kHz Default Format and a reconnect, and gains nothing
audible to expect: LDAC's bitrate stays capped at 990 kbps.

## Tests

- `core_test` **58,271** checks (58,106 at v0.1.0.36), 0 failures:
  - lane P, `TestRemoteConfiguredQuality`: the MR5's own LDAC choice (48 kHz stereo) gets HQ,
    990 kbps, with its IEs untouched and the same quality the chooser picks from the MR5's
    capabilities (44.1 / 48 / 96 kHz); MQ, adaptive (SQ) and a basic-rate radio (MQ,
    330 kbps); AAC 320 kbps VBR from the receiver becomes CBR as preferred, with the IEs
    rewritten; a 192 kbps VBR preference; never above the receiver's own 256 kbps; SBC at
    the receiver's maximum, a saved 35 / 10 within 2..53, never above the maximum or below
    the receiver's minimum, and the 350 kbps basic-rate budget;
  - `TestConfigPolicy`: the setting's defaults, values stored as sent up to 1000 ms, clamping,
    a codec word that is no codec, the reserved words untouched;
  - lane T, `TestTSinkDelay`: the ABI layout, the floors and defaults per codec, reports at
    and below the floor, the MR5's 25 / 364 ms and a fixed 280 ms, a fixed value for one
    codec or any codec, no stream, the packed setting, and the presentation position
    following a live change (above).
- `pnctl_test`: the sample-rate decoder for every codec; the sink-delay texts for every
  source, an ignored report, tenths of a millisecond and older drivers; the version gate;
  the keys and their refusals; export and import (edited lines over the blob); presets and
  automatic codecs keeping the speaker delay, accepted unchanged by the driver's validation;
  the expected applied delay for the lag range.
- `latency_test` (new, `build.ps1 -Target latency-tests`) **151** checks on synthetic
  recordings: the signal and schedule; known lags including the MR5's 267.6 ms late and
  98.6 ms early; a late stream start; noise, quiet chirps, missing chirps (6 of 8 still
  measure, 5 do not), reflections and mains hum; clock and packet-timestamp jitter; sparse
  clock samples; a lateness longer than a gap and alias candidates; the moved lag range (a
  fixed 1000 ms on a 280 ms speaker); several microphone channels; noise, silence and clicks
  rejected; a delay change during the run rejected (also 6 / 2 and 2 / 6 splits) while one or
  two strays are not; a spread above 20 ms; no clock, no recording; 8 kHz and 11.025 kHz
  microphones refused; the report text; and the speaker delay from a run with every refusal.
  It plays and records nothing: its only device calls are a read-only microphone list and
  two measurement calls with endpoint ids that do not exist, refused before anything opens.
- `build/verify.ps1` runs `latency_test` (`speaker-delay-analysis`) and its AddressSanitizer
  build (`asan-speaker-delay-analysis`): 36 software checks (34 at v0.1.0.36).

## Hardware results (15 October 2026)

Windows 11, Qualcomm FastConnect 7800, one PC, EDIFIER MR5. Microphone: a USB-C headset
microphone, 48 kHz mono float, raw mode.

### Receiver-chosen quality

| Driver | Stream | Observed result |
|---|---|---|
| 0.1.0.23 | the MR5 reconnected by itself | LDAC at **660 kbps (ceiling 660, SQ)** under an HQ preference |
| 0.1.0.24 | PN-started | LDAC 48 kHz at **990 kbps** (ABR range 330-990) |
| 0.1.0.24 | after a power cycle of the MR5 (receiver-initiated) | **990 kbps**, ceiling 990 |

No drops in steady streaming.

### Delay reports

The MR5 sent **25.0 ms** right after it set up the stream itself and **364.0 ms** later. With
the report applied as sent (before 0.1.0.25), the microphone heard the sound 267.6 ms after
the clock with the 25 ms report (8 chirps, within 3.5 ms) and 86-99 ms before it with the
364 ms report ([above](#why-the-mr5s-reports-against-a-microphone)).

### Ten stream starts

Driver 0.1.0.25. The MR5's real delay (the applied delay plus the measured lateness) over 10 normal stream
starts:

| Start | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| Speaker delay (ms) | 267 | 326 | 297 | 329 | 282 | 324 | 340 | 330 | 304 | 306 |

Median about **315 ms**, range 267-340 ms. Within one stream the delay was steady (chirp
spreads 2-9 ms); between stream starts it varied by about +-30 ms. One further run measured
**693 ms**: the first run right after a browser had been playing, not counted above. The
Balanced and Music profiles made no meaningful difference, and the batch had no ring
overflows, latency trims or transport waits.

### A fixed 315 ms for LDAC

`pna2dpctl set -d 1 sinkdelay=315 sinkdelay.codec=ldac`, then two checks:

| Check | Sound against the clock | Chirps | Spread |
|---|---|---|---|
| 1 | **27.4 ms early** | 8 of 8 | 7.9 ms |
| 2 | **16.8 ms late** | 8 of 8 | 3.2 ms |

The status estimate read 317 ms. Before, with the reports: 24-97 ms early with the 364 ms
report (over the ten starts above) or 268 ms late with the 25 ms report.

### The microphone's own timing

A measurement compares the speaker's sound with the microphone's timestamps, so the
microphone's own timing error is part of every result. A self-check laid the headset's
earbud against its own microphone (playback and recording on the same USB device, almost no
acoustic path): the chirps came **51 ms** after that device's clock (8 chirps within
0.6 ms). That is the device's playback and recording delay together; the microphone's share
is unknown but at most about 50 ms. Every speaker delay above can therefore read up to about
50 ms high: the MR5's real median is between about 265 and 315 ms, about 290 ms if the
error is shared evenly.

### A fixed 290 ms for LDAC

The owner then set 290 ms (`pna2dpctl set -d 1 sinkdelay=290 sinkdelay.codec=ldac`), about
25 ms below the measured median, to centre that uncertainty: if the microphone reads high
the sound is on time, if not it is about 25 ms late, and late sound is noticed less than
early sound. Check: the sound came **0.5 ms** after the clock (8 of 8 chirps, spread
5.6 ms).

### SBC on the MR5

For a delay test only, LDAC was disabled (`pna2dpctl set -d 1 disable=ldac`; re-enabled
afterwards, and the configuration compared identical to the saved copy). SBC at 48 kHz,
357 kbps: real delay **265 ms and 340 ms** in two runs; the speaker reports 250 ms. A third
run started while the codec changed and was rejected by the consistency check. SBC is no
faster than LDAC on the MR5, so it is no low-latency choice there.

### Wired reference

For comparison, the PC's own 3.5 mm line-out (the motherboard's Realtek audio) into the MR5's
analog input measured **67.1 ms** after that output's clock (8 chirps within 0.4 ms), with
the same microphone. That covers the MR5's processing of an analog input, the output path
and the microphone's timing (at most about 50 ms of it). Bluetooth adds roughly 250 ms on
top.

### Control panel

- **Measure with microphone** ran on hardware: the progress texts went through "Listening
  for the last chirp...", and the result read "Measured 306 ms (8 of 8 chirps, spread
  10 ms). Apply changes to use it."; another panel run gave 316 ms.
- The owner's first panel attempt showed no result. This is not explained; the driver's last
  error at that time was 0xC00000B5 (Device Timeout).
- `build/check-panel-text-fit.ps1` on the running panel: Status page, 94 texts, none cut off
  at the normal or the minimum window size in the idle state. With four flags ("audio
  running, delay reporting, remote initiated, in range") and a long Last error message,
  those two rows end in an ellipsis at the minimum window size (end-ellipsis controls, by
  design). Headset page (no stream, Fixed 290 ms for LDAC): 28 texts, none cut off at the
  normal or the minimum window size.

### Installation

Drivers 0.1.0.24 and 0.1.0.25 installed over the earlier build with audio stopped, without a
reboot.

## How to measure well

- **Start from silence.** Stop playback in every application and wait a few seconds: the
  measurement needs a fresh stream, and a delay change applied in a running stream settles
  over seconds. The 693 ms outlier was the first run right after a browser had been playing.
- **The microphone:** a USB or wired microphone that records at 16 kHz or more, 10 to 30 cm
  from the speaker, allowed for desktop apps (Windows Settings > Privacy & security >
  Microphone). A quiet room and a normal listening volume are enough.
- **Measure more than once.** The MR5's delay changes by about +-30 ms between stream
  starts, so one measurement can be off by tens of milliseconds. Measure three to five
  times, type the middle value (the median) into the **Fixed** box and press **Apply
  changes**. With `pna2dpctl`, run `measure-delay` several times without `apply`, then
  `pna2dpctl set -d N sinkdelay=<median> sinkdelay.codec=<codec>`.
- **Allow for the microphone.** Its own timing is part of the result: up to about 50 ms
  with the USB headset microphone used here (see [the microphone's own timing](#the-microphones-own-timing)).
  A value 20 to 30 ms below the median is a safe choice, because sound that comes slightly
  late is noticed less than sound that comes early.
- **Check it.** With the fixed value applied, a new measurement should report the sound
  within a few tens of milliseconds of the clock (the 315 ms check: 27.4 ms early and
  16.8 ms late).
- **One codec.** A measured value applies to the codec that played; the speaker's other
  codecs stay automatic.

## Verification

Software, release build (driver 0.1.0.25 / application 0.1.0.38): `build/verify.ps1 -Sign
-Sanitize -Interop` passed all **36** software checks: `core_test` **58,271** checks;
`latency_test` **151** checks, also under AddressSanitizer; `pnctl_test`; the signed driver
package, InfVerif, ApiValidator, the AddressSanitizer builds, the independent SBC and plugin
decoders, the installer payload and the panel's start-up; the audio-quality harness
**1,252** jobs and 24 FFmpeg baselines (1,276 in all), **0** defect flags, 0 global flags
(known issue #31 within its bounds), all **905** native-rate jobs bit-identical to the
0.1.0.21 baseline. Hardware playback is outside that script; it is recorded
[above](#hardware-results-15-october-2026).

Final installer check (15 October 2026, after the docs): `verify.ps1 -SkipBuild -Interop` on the
release installers passed all 24 software checks and `build/test-windows-mode.ps1` passed its
11 checks. The driver package in both installers is byte-identical to the one verified with
`-Sign -Sanitize -Interop` and installed for the hardware tests.

## Versions

- Driver 0.1.0.25: `PNA2DP_DRIVER_VERSION 0x00010019`, `pna2dp.rc` 0,1,0,25, INF
  `DriverVer = 10/15/2026,0.1.0.25`. Driver 0.1.0.24 (`0x00010018`) was an intermediate
  build, installed on the test PC and never published.
- Application 0.1.0.38: `tools/pna2dp-panel/version.h`. Application 0.1.0.37 was an
  intermediate build, never published.
- ABI 1 and the codec-host protocol are unchanged; `PNA2DP_CONFIG` (156 bytes) and
  `PNA2DP_STATUS` (224 bytes) keep their sizes and offsets (see [ABI](#abi)).

## Limits and checks still to do

- The MR5's delay varies by about +-30 ms between stream starts (one 693 ms outlier), so a
  single measurement can be off by tens of milliseconds; a median of several runs is better.
- The microphone measurement was tried on one speaker and one microphone only.
- Receiver-initiated AAC, aptX and SBC configurations were not tested on hardware (the MR5
  offers LDAC and SBC; SBC on the MR5 was measured for its delay only). LDAC at 96 kHz was
  not played.
- Every measurement includes the microphone's own timing (up to about 50 ms with the
  microphone used here); the engine cannot remove it without a second, known reference.
- The owner's first panel measurement showed no result, unexplained.
- On the Status page the Flags and Last error rows can end in an ellipsis at the minimum
  window size.
- One fixed delay per device, for one codec or any codec.
- Not tested: the Sony CMT-SBT40D on these drivers, long sessions, sleep / wake, Driver
  Verifier and HLK, Secure Boot or Memory Integrity (HVCI) on.
- The open checks of [maximum audio quality](20-max-audio-quality.md#hardware-checks-still-to-do)
  still apply.
