# Maximum audio quality: driver 0.1.0.23 / application 0.1.0.36

Record for the [v0.1.0.36 prerelease](releases/v0.1.0.36.md): application, control panel
and installer **0.1.0.36**, custom driver **0.1.0.23**. The owner approved three
improvements: best-quality defaults (#2), latency profiles (#5) and a high-quality
sample-rate converter (#1). All three are implemented. Testing the first build on the
EDIFIER MR5 found two older defects, both fixed in driver 0.1.0.23: Windows' **Default
Format** was ignored for every PN endpoint, and sink delay reports were ignored when delay
reporting had not been negotiated (see [hardware-found fixes](#hardware-found-fixes-driver-01023)).
The intermediate builds, driver 0.1.0.22 and application 0.1.0.35, were never published;
the sections below that name them describe work that ships in 0.1.0.23 / 0.1.0.36.

Status on 14 October 2026: **software-verified, with partial hardware validation** on one
receiver (EDIFIER MR5, LDAC). See [hardware results](#hardware-results-14-october-2026),
[verification](#verification), the
[alias-free band plan](#alias-free-band-plan-14-october-2026), its
[second review](#second-review-14-october-2026-pause-drain-and-kernel-cpu) and
[hardware checks still to do](#hardware-checks-still-to-do).

## Summary

| Area | 0.1.0.21 / 0.1.0.34 | 0.1.0.23 / 0.1.0.36 |
|---|---|---|
| LDAC default | SQ ceiling (660 / 606 kbps) | **HQ** ceiling (990 / 909 kbps); ABR steps to SQ / MQ |
| AAC default | sink maximum (256 kbps when the sink states none), VBR allowed | sink maximum **up to 320 kbps, CBR** (the MTU cap still applies) |
| SBC default | receiver's maximum bitpool | unchanged (receiver's maximum; joint / stereo, not dual channel) |
| Saved settings | any failed load saved the defaults over what was stored | defaults are saved **only when nothing is stored** |
| Buffering | target / maximum in milliseconds | **latency profiles**: Music 150 / 450, Balanced 60 / 250 (default), Low latency 30 / 120, or Custom |
| ABR with a larger buffer | same rules for every buffer | backlog under the profile's gate, or a backlog the link is draining, is not congestion |
| Best quality preset | SBC bitpool 53, 80 / 300 ms | receiver's maximum bitpool, **Music** buffering |
| Repeated trims in one stall | each trim took a new crossfade source (could click) | a crossfade not yet started keeps its source |
| Sample rates | engine rate = codec rate, or exactly 2:1 (96 -> 48, 88.2 -> 44.1 kHz) | **any of 44.1 / 48 / 88.2 / 96 kHz to 44.1 / 48 kHz**, fixed point, **alias-free** (no alias or image above -122 dB), THD+N -140.4 dB from 24-bit; native paths bit-identical |
| Codec choice | a codec without the Windows rate was skipped (e.g. aptX HD at 48 kHz only from a 44.1 kHz engine: SBC) | the priority list decides; the Windows rate is preferred, otherwise converted |
| Windows format change 44.1 <-> 48 kHz | AVDTP RECONFIGURE (an interruption) | the stream stays; the pump converts |
| 2:1 decimation | 103-tap half-band, at least 80 dB from 0.55 fs_out (its transition band aliased into 20..24 kHz) | symmetric FIRs, 195 taps from 96 kHz and 347 from 88.2 kHz, at least 122 dB from fs_out / 2 (24-bit SNR against the ideal 99.6 -> 141.1 dB) |
| Status | `HALF_RATE` flag | also `RATE_CONVERTED` and the Windows rate (`EngineSampleRate`) |
| Windows Default Format | ignored: the driver proposed its first format (48 kHz) and Windows used it | **honoured**: Windows opens the format chosen in Sound settings |
| Sink delay report | used only when delay reporting was negotiated, else a per-codec default | **used whenever the receiver sends one**; the default only when none arrives |

## #2 Best-quality defaults

**One source of truth.** The defaults, the validation and the latency profiles live in
`driver/pna2dp/inc/config_policy.h`, a pure header built by the driver, the tools and the
tests. `RegCfgDefaults` / `RegCfgValidate` (driver) and `pn::ConfigDefaults` (tools) are
one-line wrappers, so "identical to the driver's defaults" holds by construction. The
validation's clamps are unchanged: target 20..400 ms, maximum at least target + 20 and at
most 500 ms (the 600 ms ring less 100 ms headroom; `regcfg.cpp` asserts the relation).

**Defaults for a device without a saved configuration:**

- Codec priority LDAC > aptX HD > AAC > aptX > aptX LL > SBC (unchanged; aptX HD before aptX).
- LDAC **HQ** ceiling, ABR on, automatic input format. `A2dpConstrainBasicRadio` still
  forces MQ on a radio without EDR, and LDAC still needs a media MTU of at least 679.
- AAC **320 kbps CBR**: `A2dpSelectConfig` takes the smaller of the sink's stated maximum
  and 320 kbps (320 kbps when the sink states none), and the pump caps it so every access
  unit fits one media packet. The ABR rungs below the ceiling are unchanged.
- SBC `MaxBitpool` 0, the receiver's maximum within the MTU and baseband plan. The
  channel-mode default is unchanged: dual channel by default would reach known issue #31
  (the dual-channel ABR floor at bitpool 8 is audibly distorted), and no safe way to avoid
  that was found within this release.
- ABR on, automatic adapter policy on, Balanced buffering (60 / 250 ms), floor 128 kbps.

**Saved settings are never overwritten.** `DeviceStart` used to save the defaults whenever
`RegCfgLoadDevice` failed, including when a stored configuration existed but could not be
read (another ABI's size, a value the validation rejects, a transient registry error). Now
it saves only for `STATUS_OBJECT_NAME_NOT_FOUND` / `STATUS_OBJECT_PATH_NOT_FOUND`
(`PnConfigShouldSaveDefaults`); otherwise it uses the defaults in memory, logs a warning
and keeps the stored value. The first **Apply** (`SET_CONFIG`) saves as before.

Existing devices keep their saved settings, including an LDAC SQ ceiling. To move a
device to the new defaults: **Adapter > Auto quality + codecs** (or `pna2dpctl auto`),
the **Driver defaults** preset, or the **Best quality** preset.

**Tools.**

- `pn::UseAutomaticCodecs` copies the defaults' codec sections and priority (it already
  used HQ and 320 kbps CBR) and keeps volume and buffering.
- Presets (`pn::ApplyPreset`):
  - **Best quality**: LDAC, aptX HD, AAC, aptX, SBC; LDAC HQ with 24-bit input; AAC
    320 kbps CBR; SBC at the receiver's maximum bitpool (53 used to lower receivers that
    offer more); **Music** buffering; ABR on; dual channel and SBC XQ off.
  - **Driver defaults (best quality, balanced buffering)**: the full defaults, keeping the
    volume. This is the former "Balanced (driver defaults)" entry (same position).
  - **Low latency**: the Low latency profile (30 / 120 ms, as before).
  - **Compatibility** and **SBC XQ**: Balanced buffering, as before.

## #5 Latency profiles

### What the buffer settings did before

- WaveRT writes into a 600 ms ring at the engine rate. While the link keeps up, the pump
  drains every whole encoder block at once, so the steady-state delay is about one block
  plus the writes in flight, whatever the target. The ring is not a pre-buffer.
- Backlog builds only during a radio stall or while the link is slower than the stream.
  When the ring exceeds the effective maximum, the latency guard trims it to the target
  with a 4 ms crossfade.
- ABR (one window every 250 ms): **failed** writes; **pressure** (packets dropped on a full
  TX queue); **backlog** (TX queue full and ring peak above the target); **slow** (at least
  2 and 10 % of the writes slower than their media time plus 10 ms, or a full queue that
  delivered less than 90 % of real time). A reason held for 1 s, at least 2 s after the last
  change, lowers the level; a trim while the queue is full lowers it at once.
- With 60 / 250 ms, a 300 ms radio stall trims (300 > 250) and causes an urgent
  downgrade: LDAC drops from HQ to SQ for at least 30 s.

### Profiles

| Profile | Target / maximum | Use |
|---|---|---|
| Music | 150 / 450 ms | Rides out radio hiccups without lowering quality. Part of the Best quality preset. |
| Balanced (default) | 60 / 250 ms | Exactly the 0.1.0.21 behaviour. |
| Low latency | 30 / 120 ms | Video calls and games. |
| Custom | any other pair | Your own values. |

A profile is only the `TargetLatencyMs` / `MaxLatencyMs` pair: `PNA2DP_CONFIG` is unchanged
(same size, ABI 1, no reserved words used), and the driver never stores a profile id.
Every new rule derives from the effective target, so custom values scale too. A profile
change applies live (`DeviceSetConfig` already updates the pump's two values) and never
renegotiates the stream.

**Why Music is not the default.** In steady state every profile has the same delay. Music
differs only after a hiccup: up to 450 ms of backlog is kept instead of trimmed, and it
persists until the link has spare capacity to drain it. Players that use the presentation
position keep A/V sync (it includes the ring), but games, calls and system sounds do not
re-sync, so the extra delay after a hiccup could be noticed. Music is one click away and
is part of the Best quality preset.

### ABR rules that scale with the buffer

`PN_ABR_SAMPLE` (internal, not ABI) gains `SoftGateUs`, `RingStartUs` and `RingEndUs`;
the pump's `AbrTick` fills them from the ring latency at both ends of the window.

- `PnAbrSoftGateUs(target) = (target - 60) ms` above a 60 ms target, else 0. A gate of 0
  is exactly the 0.1.0.21 rule set, so Balanced and Low latency are unchanged.
- With a gate, a **slow** reason counts only while the TX queue was full **and** the backlog
  exceeded the gate (90 ms for Music).
- With a gate, a window whose ring shrank by at least 10 ms without a trim is **draining**:
  the link has spare capacity again, so neither backlog nor slow counts.
- Failures and drops always count. The urgent rule (a trim with the queue full), the 1 s
  confirmation, the 2 s spacing and the recovery timers are unchanged.

Simulation (`core_test`, LDAC 990 / 660 / 330 kbps, 10 ms steps, 250 ms windows):

| Link | Balanced | Music |
|---|---|---|
| 1.25x capacity, 300 ms stall every 8 s, 120 s | 14 trims, 2 downgrades | 0 trims, 0 downgrades, peak backlog 310 ms (the 0.1.0.21 rules at 150 / 450: 1 downgrade) |
| Capacity drops to 0.8x | downgrade after 1000 ms | downgrade after 1250 ms, 0 trims |

### SBC packing

SBC packets are sized to about a third of the latency target. `PnSbcPackingTargetMs`
caps the value packing uses at 80 ms (the largest value shipped before), so every earlier
setting packs as before and Music does not produce 50 ms packets, where one lost packet
would cost 50 ms of audio. It is used by `PumpStartStream` (`A2dpSbcPlanStream`),
`PumpSbcRepack` and the quality harness.

### Configuration, command line and panel

- Text export (`pn::ConfigToText`) writes `latency.profile=music|balanced|low|custom`, and
  `latency=` / `maxlatency=` only for Custom, so editing the profile line is not overridden
  by later lines. Import accepts `latency.profile=` (also `lowlatency`; `custom` changes
  nothing; unknown names are ignored); `latency=` and `maxlatency=` still work, later lines
  win, and the blob still restores everything exactly.
- `pna2dpctl set latency.profile=music` and `pna2dpctl set preset=best|defaults|lowlatency|compat|sbcxq`
  (volume kept; keys apply left to right). `pna2dpctl config` prints
  `latency : music (target 150 ms, max 450 ms), ABR floor ...`.
- Panel **Transport** tab: a **Listening profile** selector (Music, Balanced, Low latency,
  Custom) above the two sliders. Choosing a profile sets both values; editing a value or
  moving a slider selects the matching profile or Custom. The maximum slider now runs
  40..500 ms, the driver's limit (it showed 1000), and the panel clamps to 500 ms as the
  driver does. Import and export use the text format above.

## #1 Sample-rate converter

### What changed

Before, `PumpStartStream` accepted an engine (WaveRT) rate equal to the codec rate or
exactly twice it (C8, the 2:1 decimator, then a half-band), and the codec choice and every Windows
format change followed from that: a codec without the Windows rate was skipped, and a
Windows change between 44.1 and 48 kHz renegotiated the stream (AVDTP RECONFIGURE). The
pump now converts in fixed point (no floating point in the kernel; Q28 coefficients, 64-bit
accumulators, linear phase) between these rates (`pcm.h` `PnPcmRatePlan`):

| Engine to codec | Stage 1 | Stage 2 | Stopband from | Group delay | Latency reported |
|---|---|---|---|---|---|
| r to r | - | - | - | 0 (the native path, untouched) | 0 |
| 96 to 48 kHz | 2:1 FIR, 195 taps | - | 24 kHz | 96 engine frames (1.00 ms) | 97 frames |
| 88.2 to 44.1 kHz | 2:1 FIR, 347 taps | - | 22.05 kHz | 172 frames (1.95 ms) | 173 |
| 44.1 to 48 kHz | - | polyphase L/M = 160/147 | 22.05 kHz | 86.49 frames (1.96 ms) | 86 |
| 48 to 44.1 kHz | - | polyphase 147/160 | 22.05 kHz | 93.99 frames (1.96 ms) | 94 |
| 88.2 to 48 kHz | polyphase 160/147 into 96 kHz, 17 taps | 2:1 FIR, 195 taps | 24 kHz (stage 1 from 72 kHz) | 96.69 frames (1.10 ms) | 98 |
| 96 to 44.1 kHz | polyphase 147/160 into 88.2 kHz, 21 taps | 2:1 FIR, 347 taps | 22.05 kHz (stage 1 from 66.15 kHz) | 197.71 frames (2.06 ms) | 199 |

Any other pair is not converted, as before: 32 / 16 kHz engines, 176.4 / 192 kHz, and
conversion up to 88.2 / 96 kHz (an LDAC receiver at 96 kHz only). The reported latency
(`DelayEngineFrames`, part of the pipeline latency and so of the presentation position) is
the group delay plus, for a 2:1 stage, the wait for the second frame of a pair. The first
design (13 October) reported 89, 89, 42, 46, 173 and 181 frames; the alias-free filters
report 97, 173, 86, 94, 98 and 199, at most 2.07 ms (96 to 44.1 kHz). The alias-free plan's
first build ran the cascades 2:1 first (346 and 285 frames, 3.9 ms); the
[second review](#second-review-14-october-2026-pause-drain-and-kernel-cpu) reversed them.

### Filters

**The alias-free band plan (owner decision, 14 October 2026).** Every conversion passes
0..20 kHz with a ripple of at most 0.01 dB and stops from the **lower of its input and output
Nyquist frequencies** (22.05 kHz whenever 44.1 kHz is involved, 24 kHz for 96 and 88.2 to
48 kHz) by at least 110 dB, target 120 dB: no alias or image of any input frequency may come
out above -110 dB. In a cascade each stage protects the final band on its own; stage 1 may
alias only into a region the next stage removes. The first design (13 October) stopped from 24.1 kHz
(passband 0.4535 fs, stopband from 0.5465 fs, the usual 44.1 kHz band plan) and its half-band
was symmetric about fs_out / 2, so content between 22.05 and 24.1 kHz aliased or imaged into
20..24 kHz: a 48 kHz source's 22.5 kHz came out at 21.6 kHz only 13 dB down, 44.1 to 48 kHz
images of 21.5 kHz 15 dB down (review #2). That design and its transition-band allowance
are gone; see [the redesign](#alias-free-band-plan-14-october-2026) for the record.

- **Design.** Kaiser-windowed sinc, design attenuation A = 123 dB (beta 12.5959), the cutoff
  at the centre of the transition band, the length from Kaiser's estimate
  (A - 7.95) / (14.36 df) + 1. A = 123 dB leaves every quantised table's first stopband
  sidelobe at least 122.0 dB down (A = 120 dB left the stopband edge at -119.5 dB). The
  cascades' short stage 1 is designed for A = 150 dB (see below).
- **Polyphase stages.** 160/147 (44.1 to 48 kHz) and 147/160 (48 to 44.1 kHz), prototypes at
  F = Fin x L = 7.056 MHz, passband 20 kHz, stopband 22.05 kHz, cutoff 21.025 kHz: 160/147
  173 taps per phase (prototype N 27,679, 110,720 bytes), 147/160 188 taps per phase
  (N 27,635, 110,544 bytes), twice the taps of the first design (84 / 92): the transition
  band is half as wide (2.05 kHz instead of 4.1 kHz).
- **2:1 stages: symmetric FIRs instead of the half-band.** A half-band filter's response is
  exactly -6 dB at a quarter of its input rate, which is the output's Nyquist frequency, so
  it can never stop there. Two odd-length linear-phase FIRs replace it: from 96 kHz
  (96 to 48 kHz, and stage 2 of 88.2 to 48 kHz) 195 taps, passband 20 kHz, stopband 24 kHz;
  from 88.2 kHz (88.2 to 44.1 kHz, and stage 2 of 96 to 44.1 kHz) 347 taps, passband 20 kHz,
  stopband 22.05 kHz. Each output costs Center + 1 multiplies (98 / 174: the symmetric pairs
  share a tap). The plan picks the FIR of the rate it decimates (`PN_PCM_RATE_PLAN.Dec`); a
  state set up with only `Decimate = 2` takes the 88.2 kHz one, whose band edges (passband
  0.2268, stopband from 0.25 of the input rate) hold at 96 kHz too.
- **Cascades: the polyphase stage first.** 88.2 to 48 kHz converts 160/147 into 96 kHz,
  then takes the 96 kHz 2:1 FIR; 96 to 44.1 kHz converts 147/160 into 88.2 kHz, then takes
  the 88.2 kHz FIR. The 2:1 FIR stops from the final Nyquist frequency on its own, so the
  polyphase stage only has to stop what would fold onto the final band at its own output rate
  fmid: from fmid - fout / 2 = 0.75 fmid (72 kHz, 66.15 kHz); everything between 20 kHz and
  that edge lands in the 2:1 stopband. Its transition band is 46..52 kHz wide, so its
  prototypes (F = 14.112 MHz) are short: 17 taps per phase (N 2,719, 10,880 bytes) and 21
  (N 3,087, 12,348 bytes). Being cheap, they are designed for A = 150 dB (146.5 / 149.3 dB
  measured): their stopband holds the images of the whole passband (k Fin -+ f), which fold
  into the final band, and at 123 dB those images summed to a THD+N of -128 dB. The alias-free
  plan's first build ran the 2:1 FIR at the engine rate first (88.2 kHz: 347 taps), then a
  173 / 188-tap polyphase stage, at 2.5 times the multiplies for 88.2 to 48 kHz.
- **Quantisation.** Every polyphase phase is scaled to sum to exactly 2^28 (rounded half away
  from zero, the residual on the phase's largest tap), so DC and digital silence are exact and
  no phase has its own gain; taps are stored oldest input first. A 2:1 FIR sums to exactly
  2^28 with the residual on its centre tap, so it stays exactly symmetric; it is stored from
  the centre (`Coef[0]` the centre, `Coef[k]` the pair at centre +- k).
- **Overflow.** Inputs are S32 units held in 64 bits within +-2^33 (the +-4.0 float limit).
  Every phase's sum of |taps| is at most 2.744 x 2^28 (160/147; 147/160 2.553; the cascades'
  stage 1 1.746 and 1.714), the 2:1 FIRs' 2.227 (96 kHz) and 2.447 (88.2 kHz); the generator
  and `core_test` assert at most 3.5 x 2^28, and any sum below 4 x 2^28 keeps every
  accumulator, the rounding term included, below 2^63. A cascade's polyphase output
  overshoots by up to 1.75x and is limited to +-2^33 before it feeds the 2:1 stage:
  unclamped, 1.746 x 2^33 x 2.447 x 2^28 = 2^63.1 would pass 2^63. Measured worst-case sign
  patterns reach 2^62.46 (160/147), 2^62.35 (147/160), 2^62.15 / 2^62.29 (the 2:1 FIRs) and,
  in the cascades, 2^61.80 / 2^62.01 per stage.
- **Generated, not computed in the kernel.** `build/gen-resampler-coeffs.py` (standard
  library only, about 11 s) designs the tables in double precision, measures the shipped
  integers and writes `driver/pna2dp/inc/resample_coeffs.h` (committed; `--check` verifies it
  is current). It refuses to write a header that misses the plan: a dense scan of the whole
  stopband of every table (an FFT of the zero-padded prototype, 2^20 points, 6.7 Hz apart at
  7.056 MHz and 13.5 Hz at 14.112 MHz, 2^16 for the 2:1 FIRs, the eight largest peaks
  refined), at least 120 dB (`core_test` asserts 140 dB for the cascades' stage 1); the
  passband ripple, at most 0.0001 dB; and an explicit alias / image check of the six
  conversions as the converter runs them: for input tones every 10 Hz from 19 kHz to the
  input's Nyquist frequency and every 100 Hz below, every output component except the tone
  itself, from the integers through the polyphase identity, at most -110 dB, and each
  conversion's passband ripple at most 0.0001 dB. `pcm.cpp` includes the header once; the
  245,580 bytes of `const` tables (108,036 before) sit in `.rdata`, which is not pageable, so
  the pump thread never faults on them. A 123 dB Kaiser design in fixed point would need
  40-bit sine and Bessel approximations; baked integers are deterministic, reviewable, need
  no initialisation or allocation, and the tests measure exactly what ships.

### Conversion

- `PN_PCM_STATE` gains the polyphase window (`RsHistory`, written twice for a linear
  window), its phase (`RsPhase` = n M mod L of the next output) and the inputs still needed
  for it (`RsPending`); `PnPcmStateResetRate` starts each stream fresh. The native and 2:1
  loop is unchanged; a polyphase stream takes its own branch (`ConvertResampled`), so the
  native code is bit-exact by construction.
- Every input frame takes the native steps (NaN / Inf concealment, the mix to mono, the
  trim crossfade, counted in engine frames), then the polyphase window; a polyphase output
  is computed as soon as its last input is in and, in a cascade, feeds the 2:1 stage
  (limited to +-2^33), whose output is the converter's once a pair is in. A polyphase output
  due after a call's last output frame waits for the next call (the same result) unless more
  of the call's input must enter the window first. Everything stays in S32
  units, 64-bit and unsaturated, until the single store: S16 gets TPDF dither, S24 is rounded
  on a converted stream (`Round24`; native streams keep truncating), S32 and F32 are exact.
  Digital silence stays exact zeros once the histories are empty.
- **Exact framing.** `PnPcmInputFramesFor(state, n)` is the exact number of engine frames
  that make n codec frames (64-bit integer arithmetic); the pump hands over exactly that
  with room for exactly n, and the converter consumes all of it. `PnPcmMaxInputFrames` is
  the bound over every state (buffer sizing and the wake level): for SBC 15 x 128 frames at
  96 to 44.1 kHz from float that is 33.4 KB, so 14 frames fit the 32 KB block.
- **A mid-programme start fades in before the filters.** At a stream start after an outage
  the first block used to ramp its output from silence. On a converted stream the filters
  see a full-level onset in their empty history, which leaves them one filter delay later,
  when the ramp has risen: a band-limited step (aptX HD turned it into a +28 dB click at a
  converted restart in the quality harness). Converted streams now ramp the first block's
  input instead (`RampIn`); native streams are unchanged.

### Pump, status and policy

- `PumpStartStream` plans the conversion (`STATUS_NOT_SUPPORTED` for a pair it cannot
  bridge) and resets the converter; the steps take `PnPcmInputFramesFor` frames;
  `PcmFramesConsumed`, the ring and trims stay in engine frames and RTP time in codec
  frames. LDAC with the automatic format takes 32-bit input on a converted stream.
- **The end of the sound at a pause.** `PumpFinishStream` pads the waiting PCM to whole
  blocks of the wake level, the most one block takes, plus `PnFinishExtraBlocks` blocks of
  silence: the codec's own flush (one block, AAC two), and on a converted stream
  ceil((DelayEngineFrames + block - 1) / block) more. A converted stream's blocks take up to
  block - 1 frames less than the wake level, so the pump can stop that far short of the
  padding's end, and the converter holds `DelayEngineFrames` of what it took; with these
  blocks the pump takes at least the rendered audio, the converter's delay and the
  equal-rate flush, so every rendered frame leaves the converter and the encoder codes it
  (SBC needs 73 more codec frames, 37 with 4 subbands, aptX 90, AAC 1600, as the harness
  calibrates them) before the stream goes idle. Fewer blocks left the sound ending mid-fade,
  its rest coming out after the resume: with one extra block (0.1.0.21) in the converter;
  with one more only for a polyphase stage (this round's first rule) in the SBC encoder when
  one SBC frame fills a packet ([second review](#second-review-14-october-2026-pause-drain-and-kernel-cpu)).
  `core_test` simulates the pump's stop with the production framing over every converter
  phase, every backlog up to the 500 ms latency maximum, the residuals that bound the
  margin and every production block size (SBC 128 n and 64 n, aptX 32 n from 128 to 640,
  LDAC 256 / 512, AAC 1024 codec frames): 6.9 million cases, none short of the converter,
  of the codec's delay or of the equal-rate flush; the smallest margin 38.1 codec frames past
  the codec's own delay.
- Status: `PNA2DP_STATUS_FLAG_RATE_CONVERTED` (0x1000) while the engine rate differs from
  the codec rate, `HALF_RATE` (0x800) for the 2:1 stage alone, and
  `PNA2DP_STATUS.EngineSampleRate`, a former reserved word. The panel's Status page shows
  it on a **Windows rate** row ("44100 Hz, converted to 48000 Hz"); `pna2dpctl status`
  prints "48000 Hz (Windows 44100 Hz, converted), 2 ch, 32-bit in".
- **Codec choice** (`transport_policy.h`, `PN_RATECONV_ANY` = `PN_PUMP_RATE_CONVERT`): the
  stream takes the engine rate whenever the sink has it (no conversion); otherwise exactly
  half of 96 / 88.2 kHz, then 44.1 or 48 kHz, the engine rate's own family first. Every
  codec with 44.1 or 48 kHz is usable from any of the four engine rates, so the priority list
  decides: an aptX HD sink at 48 kHz only is chosen from a 44.1 kHz engine (it was skipped
  for SBC), and an LDAC sink at 44.1 / 48 kHz runs LDAC from 88.2 or 96 kHz.
- **Format changes** (`PnStreamBridged`): a stream that has already played (the pump started
  on its current configuration, `ActiveStreamed`) and whose rate the converter bridges from
  the new engine rate stays as it is: no RECONFIGURE when Windows switches between 44.1 and
  48 kHz. A stream that has carried no audio since it was configured, the receiver's own
  SET_CONFIGURATION or ours made with no audio open (auto start on connect), goes to the
  engine rate before its first START, where a RECONFIGURE interrupts nothing (review #1, #3,
  #4). A bridged stream that needs a RECONFIGURE anyway, for a channel-mode preference, is
  reconfigured to the engine's own stream rate, which ends the conversion. A hi-res engine
  rate above 48 kHz that the sink takes natively still gets its own stream (LDAC at 96 kHz in
  exclusive mode). A bridged stream keeps its rate until it is configured again (codec
  upgrade, renegotiation, reconnect).
- With `PN_RATECONV_NONE` or `_HALF` every decision is that of 0.1.0.21 (checked over
  20,000 random inputs against a copy of the 0.1.0.21 rules).

### Format table: unchanged

The render pin still offers 48 and 44.1 kHz always (16-bit, plus 24-in-32 and float when a
hi-res codec is possible), the negotiated rate, and every sink rate only with **Expose all
sample rates**. 88.2 / 96 kHz are not offered unconditionally: the converter's outputs are
44.1 and 48 kHz, so a 96 kHz engine feeding a 48 kHz receiver costs CPU and a conversion
for nothing; Windows' shared engine runs at the endpoint's default format, and offering
hi-res rates invites formats that need conversion; and a table change raises a format
change that Windows remembers per endpoint, so existing users see none. 32 and 16 kHz are
offered only when the sink takes them, since they are not converted.

Which of these formats Windows actually opens changed in driver 0.1.0.23: the driver no
longer proposes a default format, so the user's **Default Format** choice takes effect
(see [hardware-found fixes](#hardware-found-fixes-driver-01023)).

### Cost

- Memory: 245,580 bytes of tables in `.rdata` (108,036 in the first design); `PN_PCM_STATE`
  grows from 3,368 bytes (0.1.0.21) to 17,232 bytes per device (8,776 in the first design),
  inside the non-paged pump, never on a kernel stack; no allocations. The driver image grows
  from 204,800 bytes (0.1.0.21) to 456,704 bytes (319,488 with the first design).
- CPU per second of stereo audio (S32, 1024-frame blocks, best of three; `core_test`),
  against the owner's budget for the alias-free plan (about twice the first design, the
  worst cascade at most about 6 ms in the user-mode timing). The user-mode timing is now
  exactly the code the driver runs:

  | Conversion | Multiplies | This build (user mode = kernel code) | Alias-free first build: user / kernel | First design (user / scalar) |
  |---|---|---|---|---|
  | 96 to 48 kHz | 9.4 M/s | 2.76 ms | 2.67 / 4.28 ms | 1.55 / 1.55 ms |
  | 88.2 to 44.1 kHz | 15.3 M/s | 4.03 ms | 3.83 / 7.02 ms | 1.42 / 1.42 ms |
  | 44.1 to 48 kHz | 16.6 M/s | 3.23 ms | 2.22 / 3.96 ms | 1.32 / 2.22 ms |
  | 48 to 44.1 kHz | 16.6 M/s | 3.26 ms | 2.2 / 4.0 ms | 1.31 / 2.20 ms |
  | 88.2 to 48 kHz | 12.7 M/s (was 32.0) | 3.59 ms | 5.6 / 10.5 ms | 2.41 / 3.31 ms |
  | 96 to 44.1 kHz | 19.1 M/s (was 26.0) | **4.90 ms** (0.49 % of one core) | 4.5 / 7.95 ms | 2.49 / 3.39 ms |

  The kernel figures of the first build are from the independent measurement with
  `__isa_available` forced to 0 (what the driver ran). Native paths: 0.46 ms.
- **No vector-ISA dispatch in the driver.** MSVC vectorised the 64-bit multiply-accumulate
  loops with AVX-512 behind a run-time check of `__isa_available`, which the C runtime sets
  in user mode and nothing sets in the kernel, so the driver ran the scalar fallback (dead
  AVX-512 code in the image, which would have used vector state without
  `KeSaveExtendedProcessorState` had anything set the flag) while the user-mode tests timed
  the AVX-512 path. The converter's loops (`pcm.cpp`) and the SBC filterbanks (`sbc.c`, the
  same dispatch in `sbc_analyze` / `sbc_synthesize`) are now scalar by construction
  (`#pragma loop(no_vector)`); both channels share each coefficient load (channel 1's window
  sits at a fixed distance from channel 0's) and two taps run per iteration, so the scalar
  code is faster than the old scalar fallback. `build.ps1` refuses a driver image whose map
  contains `__isa_available`. Integer results are identical either way.
- The work runs on the pump thread at PASSIVE level with no spin lock held.

### Measurements

The alias-free tables (14 October 2026). `gen-resampler-coeffs.py` on the shipped integers
(also in the generated header):

| Filter | Taps | Passband ripple 0..20 kHz | Stopband (whole band, FFT 6.7 Hz apart) | Sum of \|taps\| |
|---|---|---|---|---|
| 2:1 from 96 kHz | 195 (centre 97) | 0.000012 dB | 122.0 dB from 24 kHz (worst at 24.046 kHz) | 2.227 |
| 2:1 from 88.2 kHz | 347 (centre 173) | 0.000012 dB | 122.7 dB from 22.05 kHz (22.079 kHz) | 2.447 |
| 160/147 (44.1 to 48 kHz) | 173 per phase, N 27,679 | 0.000012 dB | 122.9 dB from 22.05 kHz (22.078 kHz) | 2.744 (phase maximum) |
| 147/160 (48 to 44.1 kHz) | 188 per phase, N 27,635 | 0.000012 dB | 122.9 dB from 22.05 kHz (22.08 kHz) | 2.553 |
| 160/147 from 88.2 kHz (stage 1 of 88.2 to 48 kHz) | 17 per phase, N 2,719 | 0.000001 dB | 146.5 dB from 72 kHz (72.244 kHz) | 1.746 |
| 147/160 from 96 kHz (stage 1 of 96 to 44.1 kHz) | 21 per phase, N 3,087 | 0.000001 dB | 149.3 dB from 66.15 kHz (72.303 kHz) | 1.714 |

Every conversion as the converter runs it (input tones every 10 Hz from 19 kHz to the
input's Nyquist frequency, every 100 Hz below; every output component but the tone itself):

| Conversion | Worst alias / image | Passband ripple |
|---|---|---|
| 44.1 to 48 kHz | -122.9 dB (22.02 kHz imaged to 22.08 kHz) | 0.000012 dB |
| 48 to 44.1 kHz | -122.9 dB (22.08 kHz aliased to 22.02 kHz) | 0.000012 dB |
| 96 to 48 kHz | -122.0 dB (24.05 kHz aliased to 23.95 kHz) | 0.000012 dB |
| 88.2 to 44.1 kHz | -122.8 dB (22.08 kHz to 22.02 kHz) | 0.000012 dB |
| 88.2 to 48 kHz | -122.0 dB (24.05 kHz aliased to 23.95 kHz by the 96 kHz FIR) | 0.000012 dB |
| 96 to 44.1 kHz | -122.1 dB (27.68 kHz: its image at 68.32 kHz, the stage-1 stopband) | 0.000021 dB |

`core_test` (lane R, the shipped integers and the converter itself; specification in
brackets):

| Check | Result |
|---|---|
| Passband ripple 0..20 kHz (<= 0.01 dB) | every table 0.000012 dB (FFT of the whole band, 2^20 points for the prototypes, 2^16 for the 2:1 FIRs) |
| Stopband from the lower Nyquist frequency (>= 110 dB, limit here 120 dB) | 160/147 and 147/160 122.9 dB from 22.05 kHz; 2:1 from 88.2 kHz 122.8 dB from 22.05 kHz; 2:1 from 96 kHz 122.0 dB from 24 kHz; the cascades' stage 1 146.5 dB from 72 kHz and 149.3 dB from 66.15 kHz (limit 140 dB) |
| Transition-band probes through every pair (<= -110 dB): tones whose alias or image lands at 22.5..23.9 kHz, and every 250 Hz from 20 to 24 kHz (26 kHz into 48 kHz from 88.2 / 96 kHz), float in, S32 out | worst alias / image -124.1 dB (96 to 48), -126.3 (88.2 to 44.1), -126.5 (44.1 to 48), -126.7 (48 to 44.1), -124.1 (88.2 to 48), -126.3 dB (96 to 44.1); everything else the converter puts out -132.0 dB or lower. The first design: -7.2 to -94.1 dB, 84 failures |
| THD+N, 997 Hz -1 dBFS (<= -110 dB) | -140.4 dB from 24-bit TPDF to S32 (the input's dither alone: -140.5 dB), -139.1 dB to rounded S24, -148.9 dB from float; level error 0.00000 dB |
| Tones 100 Hz..20 kHz through every pair | within 0.00001 dB |
| Stopband tones (30 kHz from 96 / 88.2 kHz, the 19 kHz image) | 152.3 dB worst |
| Group delay | as designed within 0.01 frame (96.0000, 172.0000, 86.4937, 93.9932, 96.6938, 197.7075) |
| Against a double-precision model of the definition | within 0.50 LSB of S32 (single stages), 1.03 / 1.08 LSB (cascades); a +-4.0 worst case saturates every output with the model's sign |
| Block boundaries | 2 s of tone plus noise in one call and in random blocks of 1..4096 frames: bit-identical (S32 and dithered S16) |
| Pause drain | 6,915,697 cases (every converter phase, backlogs up to 500 ms, every production block size): none leaves rendered audio in the converter or the encoder; smallest margin 38.1 codec frames past the codec's own delay |
| Native paths | 200 combinations hash to the 0.1.0.21 constant |

Quality harness, group "rate" (codec input against the analytic signal delayed by the
group delay; 13 configurations; transition: the worst alias / image of the four probe tones
and of the stepped sweep, float in):

| Configuration | THD+N -1 dBFS (S16 / 24-bit / float in) | -6 / -60 dBFS | Sweep 20 Hz-20 kHz SNR | 20 kHz level | Alias / image | Transition (tones / sweep) |
|---|---|---|---|---|---|---|
| SBC Sony dual 53, 44.1 to 48 kHz | -92.5 / -140.3 / -149.1 dB | -87.5 / -33.5 dB | 87.5 dB | 0.00000 dB | image 157.6 dB | -126.5 / -128.2 dB |
| SBC Sony dual 53, 48 to 44.1 kHz | -92.9 / -140.6 / -148.9 dB | -87.9 / -33.9 dB | 87.9 dB | 0.00001 dB | image 128.7 dB | -126.7 / -126.7 dB |
| SBC Sony dual 53, 96 to 44.1 kHz | -95.9 / -143.9 / -153.3 dB | -91.0 / -36.9 dB | 90.9 dB | 0.00000 dB | 142.0 / 132.0 dB | -126.3 / -126.3 dB |
| SBC Sony dual 53, 88.2 to 48 kHz | -95.4 / -143.0 / -150.5 dB | -90.4 / -36.4 dB | 90.4 dB | 0.00000 dB | 133.3 / 133.2 dB | -124.1 / -124.1 dB |
| SBC Sony dual 53, 96 to 48 kHz (2:1) | -95.7 / -143.9 / -156.5 dB | -90.7 / -36.8 dB | 90.7 dB | 0.00000 dB | alias 135.7 dB | -124.1 / -124.1 dB |
| SBC Sony dual 53, 88.2 to 44.1 kHz (2:1) | -95.5 / -143.7 / -156.3 dB | -90.5 / -36.5 dB | 90.5 dB | 0.00000 dB | alias 129.3 dB | -126.3 / -126.3 dB |
| AAC CBR, 44.1 to 48 kHz (S16 dithered) | -89.4 / -92.3 / -92.3 dB | -84.4 / -30.4 dB | 84.4 dB | 0.00000 dB | image 133.4 dB | -120.6 / -116.7 dB |
| AAC CBR, 96 to 48 kHz (S16 dithered) | -90.7 / -92.3 / -92.3 dB | -85.6 / -31.7 dB | 85.7 dB | 0.00000 dB | alias 126.4 dB | -123.8 / -116.7 dB |
| aptX HD, 44.1 to 48 kHz (S24 rounded) | - / -139.1 / -143.8 dB | -134.3 / -80.4 dB | 133.8 dB | 0.00000 dB | image 159.8 dB | -126.6 / -128.4 dB |
| aptX HD, 96 to 44.1 kHz | - / -141.5 / -144.6 dB | -136.6 / -82.6 dB | 136.3 dB | 0.00000 dB | 153.9 / 178.9 dB | -126.0 / -126.0 dB |
| aptX HD, 96 to 48 kHz (2:1) | - / -141.5 / -144.9 dB | -136.6 / -82.6 dB | 136.2 dB | 0.00000 dB | alias 156.0 dB | -123.9 / -123.9 dB |
| LDAC HQ, 44.1 to 48 kHz | - / -140.3 / -149.1 dB | -135.6 / -81.7 dB | 134.9 dB | 0.00000 dB | image 158.0 dB | -126.5 / -128.2 dB |
| LDAC HQ, 48 to 44.1 kHz | - / -140.6 / -148.9 dB | -135.9 / -82.1 dB | 135.0 dB | 0.00000 dB | image 139.1 dB | -126.7 / -126.7 dB |

The 16-bit and AAC figures are the 16-bit source's (or the 16-bit encoder input's) own
noise; the -60 dBFS figures likewise. The AAC transition figures are the floor of that
input's TPDF dither (the converter's own aliases are the SBC / LDAC rows' -124..-127 dB):
16-bit dither near 22 kHz measures about -126 dB re the tone in a 1.3 s fit and -117 dB in
the sweep's 180 ms steps, still under the -110 dB requirement. No 1 ms window is more than a
few dB worse than the whole (24-bit: 137..141 dB), which shows no click at a block boundary.
Before the converter, the 2:1 jobs measured 99.6 dB (24-bit, 96 to 48 kHz) and 61.0 dB
(20 kHz, 88.2 to 44.1 kHz) against the ideal; they now measure 141.1 and 90.5 dB (the latter
the 16-bit source's limit).

## Fix: repeated latency trims in one stall

The Low latency harness jobs found a defect that predates this change. During a stall
nothing is converted, so the ring can exceed the maximum again before the first trim's
crossfade has started (with 30 / 120 ms every 90 ms; with Balanced after about 190 ms,
so in stalls longer than about 440 ms). The second trim replaced the crossfade source with
the head of the newly dropped audio, which follows audio that was itself dropped, and the
splice stepped: -10.5 dB (SBC) and -14.5 dB (LDAC) re tone at the pre-codec click check,
threshold -40 dB. `PumpTrim` now keeps a crossfade that has not started
(`PnPcmFadePending`): its source continues the audio played last. The quality harness's
emulated trim does the same. The core test models both rules on a sine.

## Review fixes (14 October 2026)

An independent measurement and a three-lens review of this change confirmed five findings;
all are fixed, each with a test that failed first. They were made on the first converter
design; the [alias-free band plan](#alias-free-band-plan-14-october-2026) then replaced its
tables (#2 below).

- **#1 / #3 / #4: bridging only a stream that has played.** The first build bridged any
  convertible rate, also on a stream that had carried no audio: the receiver's own
  SET_CONFIGURATION (the Sony's 44.1 kHz joint-stereo offer under a 48 kHz engine stayed at
  44.1 kHz with conversion, and with its saved dual-channel preference was still
  reconfigured, to 44.1 kHz dual, 617 kbps, instead of 48 kHz dual, 672 kbps), and ours made
  with no audio open (auto start on connect configured LDAC 48 kHz; a 44.1 kHz engine was
  then converted for the whole session). A RECONFIGURE before the first START interrupts
  nothing, so requirement (a) applies: the engine rate. `PN_FORMAT_INPUT.Streamed`
  (`ActiveStreamed` in the device, set when `PumpStartStream` succeeds, cleared with each new
  or changed configuration and at Idle) gates `PnStreamBridged`, and a bridged plan that
  needs a RECONFIGURE anyway (a channel-mode preference) targets the engine's own stream
  rate (`PnPlanStreamFormatAt` run twice). The S-44k scenario and the Sony path now also run
  with `PN_RATECONV_ANY`, the shipped setting (they had zeroed inputs, so `NONE`); the
  Sony's receiver-initiated 44.1 kHz offer ends at 48 kHz dual channel, bitpool 53, 672 kbps
  under both `HALF` and `ANY`. A random check over 20,000 inputs asserts that every
  RECONFIGURE lands on `PnStreamRateFor(engine)` and that a stream that has not played is
  kept only at that rate. Red first: 12 checks.
- **#2: transition band.** Superseded the same day by the owner's decision for the
  alias-free band plan: the transition band no longer leaks, and these probes now require
  -110 dB (see below). At the time, the design was kept; `pcm.h` and the generated
  header said "at least 120 dB" (the generator sampled the stopband every 250 Hz and missed
  the first sidelobe at 24.16 kHz). The generator now samples the first kilohertz every
  5 Hz and reports 118.9 / 119.7 dB and the transition band levels in the header (version 2,
  tables identical). `core_test` gained probes at 22.5, 23, 23.5 and 23.9 kHz for 48 and
  96 to 44.1 kHz and a 21.5 kHz image for 44.1 to 48 kHz (48 to 44.1 kHz had no alias probe
  at all), each bounded by the design's level and everything else at least 110 dB down; the
  quality harness gained transition-band jobs (22.5 / 23 kHz) for the four 48 kHz family to
  44.1 kHz configurations, reported as `rate_transition_alias_db` and in the summary's
  `t22.5k` column. The summary's window column (`win24`) now shows the 24-bit / float
  -1 dBFS tones; it showed the minimum over every job, including -60 dBFS 16-bit ones.
- **#5: `preset=balanced` / `preset=low`.** `PresetFromKey` accepted the latency profile
  words as preset keys, so `pna2dpctl set preset=balanced` replaced the whole configuration
  (codec order, SBC XQ / dual channel, auto start, reconnect delay) and `preset=low` disabled
  LDAC and AAC. Both are refused now; `pna2dpctl` points to `latency.profile=`. Red first:
  `pnctl_test` 1 check.
- Not changed, by measurement: the 160/147 table has two tap pairs that differ by 2 LSB of
  Q28 (the per-phase rounding residual; its effect on the phase is below 0.00001 samples);
  the 2:1 plans report about one engine frame more latency than the measured group
  delay, by design (the wait for the second frame of a pair, about 10 us); dithered 16-bit
  output at a digital-silence edge depends on the block split, the same rule as the native
  path.

Verification of the fixes (software only, nothing installed): every target builds with no
warnings (driver Release 319,488 bytes and Debug with `/WX`, InfVerif valid, ApiValidator
universal; `/analyze` 23 at its baseline of 23); `core_test` 57,607 checks, 0 failures (lane
T RED group 139 checks); `pnctl_test`, `sbc_test`, `plugin_test`, `secure_path_test` 24,
`health_test` 73, `service_failure_test` 21, `process_wait_test` 13, `boot_mode_test` 40
pass; `test-interop.py` 19 / 0 and `test-plugin-interop.py` 28 / 0; quality harness 1147
jobs and 24 FFmpeg baselines, 0 defect flags, 0 global flags (known issue #31 within its
bounds), all 905 native-rate jobs bit-identical to the 0.1.0.21 baseline;
`gen-resampler-coeffs.py --check` current.

## Alias-free band plan (14 October 2026)

**Owner decision: "make it best".** Every conversion alias-free: passband 0..20 kHz (ripple
at most 0.01 dB), stopband from the lower of the input and output Nyquist frequencies with at
least 110 dB (target 120 dB), each cascade stage protecting the final band on its own; fixed
point, linear phase, exact rate accounting, block-size independence and overflow safety kept,
native paths untouched, about twice the taps and CPU acceptable (the worst cascade at most
about 6 ms per second of stereo audio in user mode, tables at most about 300 KB). The first
design met 110 dB only from 24.1 kHz, so for a 48 kHz source every frequency that can alias
(22.05..24 kHz) lay in its transition band, and the half-band let 88.2 / 96 kHz content
alias into 20..24 kHz.

**Red first.** The tests were changed to the new requirement and run against the first
design's tables before anything else changed:

- `core_test` lane R, `TestRateConverterTransition`: through every converted pair, the tones
  whose alias or image lands at 22.5, 23, 23.5 and 23.9 kHz (the tones themselves into
  44.1 kHz; 25.5..24.1 kHz into 48 kHz from 88.2 / 96 kHz, whose own 22.5..23.9 kHz pass;
  21.6..20.2 kHz of a 44.1 kHz source) and a sweep every 250 Hz from 20 kHz to 24 kHz
  (26 kHz into 48 kHz from 88.2 / 96 kHz), the converter run on float tones: the main alias
  or image and everything else at most -110 dB. Against the first tables: **84 failures**
  (aliases and images between -7.2 dB, 24.1 kHz onto 23.9 kHz from 96 kHz, and -94.1 dB;
  "everything else" up to -17.6 dB through the 88.2 to 48 kHz cascade).
- Quality harness: the same tones (`transition<f>k_-6dBFS`) and a stepped sweep
  (`transition-sweep...`, 250 Hz steps of 200 ms) for each of the 13 converted
  configurations, float in, the alias or image fitted at its exact frequency (`RateSpurious`):
  **65 of 81 jobs flagged** against the first tables (worst -7.2 dB). The other 16 cannot
  fail: tones a 2:1 stage alone passes in band, and 24.1..25.5 kHz into 88.2 to 48 kHz,
  which the old half-band already stopped. They replace the two transition jobs per
  48-kHz-family to 44.1 kHz configuration that only checked the design's own levels.

**What changed.**

- `build/gen-resampler-coeffs.py` version 3: the band plan, A = 123 dB, the Kaiser lengths,
  the two 2:1 FIRs, and its checks (the dense FFT scan of every stopband, the ripple, the
  per-conversion alias / image and passband check); `resample_coeffs.h` regenerated.
- `pcm.h`, `pcm.cpp`: `PN_DEC2_TABLE` (`g_PnDec96`, `g_PnDec88`, stored from the centre)
  replaces the half-band (`PN_HB_*`, `g_PnHalfbandQ28`); the plan names the 2:1 FIR
  (`PN_PCM_RATE_PLAN.Dec`); the state's 2:1 window is `Dec` / `DecPhase` / `DecPos` /
  `DecHistory[2][2 * 347]` (was `HbPhase` / `HbPos` / `HbHistory`); 173 / 188 taps per phase;
  `DelayEngineFrames` 97, 173, 86, 94, 346, 285; the overflow bounds re-derived; the window
  positions and the polyphase phase advance without a division. The native path and its
  arithmetic are untouched.
- `core_test`: lane R measures every table over the whole band by FFT and regenerates it
  from the design, the transition probes above, the 2:1 model, the delays, the cost and the
  pause drain over every production block size; lane P checks both 2:1 tables (DC exact,
  ripple, at least 110 dB from fs_out / 2). Harness: the transition jobs and the stepped
  sweep (`K_STEPS`), `rate_transition_alias_db`, `rate_sweep_alias_db`, the summary's
  `trans` / `tsweep` columns.
- `PnFinishExtraBlocks` was left unchanged: the simulation then sampled four backlogs and
  checked only the converter's delay (smallest margin 28 frames). The second review found
  the encoder's own delay uncovered and a backlog above ~415 ms short of the converter (see
  below).

**Result** (this build, with the second review's cascades). No alias or image of any input
frequency above -122.0 dB in any conversion (generator, every 10 Hz near the band edge); the
converter run on tones: -124.1 dB or lower; passband 0.000012 dB per stage (0.000021 dB through
a cascade); THD+N and the native paths as before. Tables 245,580 bytes (budget about
300 KB); CPU at most 4.90 ms per second of stereo audio (96 to 44.1 kHz) in exactly the code
the driver runs (budget about 6 ms; see [Cost](#cost)); latency at most 199 engine frames
(2.07 ms). The first build of this plan (2:1-first cascades) measured -130.5 / -129.5 dB through
the cascades, 5.6 ms in user mode but 10.5 ms in the kernel's scalar loops, and 346 frames.

**Verification** (software only; nothing installed, signed or loaded, no receiver):

- Every target builds with no warnings: driver Release (433,152 bytes, `/WX`, InfVerif
  valid, ApiValidator universal) and Debug, `/analyze` 23 at its baseline of 23, tests,
  host, CLI, panel, plugins, plugin tests, quality harness.
- `core_test` **57,884** checks, 0 failures; `sbc_test`, `plugin_test`, `pnctl_test` pass;
  `secure_path_test` 24, `health_test` 73 (offline), `service_failure_test` 21,
  `process_wait_test` 13, `boot_mode_test` 40 checks, 0 failures; `test-interop.py` 19 / 0,
  `test-plugin-interop.py` 28 / 0.
- Quality harness: **1220** jobs and 24 FFmpeg baselines, **0** defect flags, 0 global
  flags (known issue #31 within its bounds); `--hashes-check`: all **905 native-rate jobs
  bit-identical** to the 0.1.0.21 baseline.
- `gen-resampler-coeffs.py --check` current.

## Second review (14 October 2026): pause drain and kernel CPU

An independent measurement of the alias-free build confirmed its band plan and found two
defects and two partial items; all are fixed, each with a test that failed first.

- **#1 The encoder's delay at a pause** (`audio_policy.h` `PnFinishExtraBlocks`). The longer
  alias-free delays (88.2 to 44.1 kHz 173 engine frames, 88.2 to 48 kHz 346) used up most of
  the silence that flushes the SBC encoder, and the drain checks counted only the converter.
  With one SBC frame per packet (128-frame blocks; high-bitpool SBC) about a quarter of the
  pauses at 88.2 to 44.1 kHz left the last ~0.7 ms of the fade-out (about -22 dB) in the
  encoder: it played just before the resumed fade-in, or was lost after a SUSPEND. At
  88.2 to 48 kHz a backlog above ~415 ms (the Music profile allows 450) also left rendered
  audio in the converter, the 0.1.0.22 #1 symptom. The rule is now general: the codec's flush
  plus ceil((DelayEngineFrames + block - 1) / block) blocks on a converted stream, which
  guarantees at least the rendered audio, the converter's delay and the equal-rate flush
  (`pump.cpp`, the harness's emulation). Native streams are unchanged.
  - `core_test` `TestRateConverterPauseDrain` now models the pump's stop exactly over every
    converter phase, every backlog up to 500 ms (it sampled 0, 3, 30 and 120 blocks), the
    residuals that bound the margin and every production block size, and requires the
    converter's delay plus the codec's own (SBC 73 / 37, aptX 90, AAC 1600 codec frames,
    the harness's calibrated delays). Red first: against the first rule 474,299 cases short
    of the converter and 988,932 short of the codec's delay (af2 red run); green: 6,915,697
    cases, none short; the first rule still fails 8,204 one-frame-per-packet SBC cases with
    the final filters, kept as a check.
  - Quality harness: `pause_unsent_ms` adds the codec's delay; new configurations
    `sbc-k-joint-bp250-88to44` / `-88to48` (one SBC frame per packet) with 16 pause instants
    each. Red first: 10 of 32 jobs flagged (PAUSE, 0.02..0.2 ms left); green: none.
- **#2 `pcm.h` comment.** `RsHistory` referred to the removed `HbHistory`; both window
  comments now state their bound (+-2^33) and why.
- **CPU in the kernel (partial).** The ~6 ms budget held only in the user-mode timing: the
  kernel ran the scalar fallback of AVX-512-dispatched loops, 10.5 ms for 88.2 to 48 kHz.
  Three changes: the loops are scalar by construction and process both channels per
  coefficient (see [Cost](#cost)); the cascades run the polyphase stage first (see
  [Filters](#filters)), 0.40 of the multiplies for 88.2 to 48 kHz and 0.73 for 96 to
  44.1 kHz; and `build.ps1` rejects a driver with vector-ISA dispatch (red first: the
  previous `pna2dp.map` listed `__isa_available` from `pcm.obj`; after the converter fix
  the check still failed on `sbc.obj`, whose filterbanks had the same dispatch). Now the worst
  conversion costs 4.90 ms per second of stereo audio in the code the driver runs (was
  10.5 ms), and the reported latency fell from 346 / 285 to 98 / 199 engine frames. The
  cascades' worst alias moves from -130 to -122 dB, the single-stage level every other pair
  already had; their transition probes measure -124.1 / -126.3 dB (were -132.4 / -137.0).
- **Documentation.** The CPU and cascade figures above replace "5.5 ms" and "-129.9 dB".

Verification (software only; nothing installed, signed or loaded, no receiver):

- Every target builds with no warnings: driver Release (456,704 bytes, `/WX`, InfVerif
  valid, ApiValidator universal, no `__isa_available` in the map) and Debug, `/analyze` 23 at
  its baseline of 23, tests, host, CLI, panel, plugins, plugin tests, quality harness.
- `core_test` **58,105** checks, 0 failures; `sbc_test`, `plugin_test`, `pnctl_test` pass;
  `secure_path_test` 24, `health_test` 73 (offline), `service_failure_test` 21,
  `process_wait_test` 13, `boot_mode_test` 40 checks, 0 failures; `test-interop.py` 19 / 0,
  `test-plugin-interop.py` 28 / 0 (SBC delay 73, aptX 90).
- Quality harness: **1252** jobs and 24 FFmpeg baselines, **0** defect flags, 0 global
  flags (known issue #31 within its bounds); `--hashes-check`: all **905 native-rate jobs
  bit-identical** to the 0.1.0.21 baseline. A separate build of HEAD's and this `pcm.cpp`
  hashes 672 native cases identically.
- An independent-style measurement program linked against `pcm.cpp`: 16,191 tones (worst
  component -122.0 dB, tones up to 20 kHz -132.7 dB or lower), THD+N as above, 112
  configurations x 23 framings bit-identical, one hour per pair with exact rate accounting
  in both pump and input-driven modes (drift below 3e-14 frames), overload with 0 sign
  flips and outputs within 1.0 LSB of a double model.
- `gen-resampler-coeffs.py --check` current (version 4).

## Hardware-found fixes (driver 0.1.0.23)

The first hardware session with driver 0.1.0.22 (see
[hardware results](#hardware-results-14-october-2026)) played LDAC HQ cleanly but showed two
defects that predate this round. Both are fixed in driver 0.1.0.23.

### Windows' Default Format was ignored

- **Symptom.** Choosing 44.1 kHz in **Sound > device > Properties > Advanced > Default
  Format** had no effect: Windows' engine kept running at 48 kHz, so a 44.1 kHz source was
  resampled by Windows and the receiver's own 44.1 kHz mode was never used.
- **Cause.** Since the first version the wave filter answered
  `KSPROPERTY_PIN_PROPOSEDATAFORMAT2` with the first entry of its format table (48 kHz).
  Windows takes a driver's answer to that property as the engine format and then ignores
  the user's Default Format, so every PN endpoint's shared-mode engine was pinned to that
  entry (48 kHz unless the active stream rate was 96 / 88.2 / 32 / 16 kHz).
- **Fix** (`audio_wave.cpp`). The property is no longer supported (the handler and its
  table entry are removed; `KSPROPERTY_PIN_PROPOSEDATAFORMAT`, the format check, stays).
  Windows now opens the format the user selects. The stream follows that rate natively when
  the receiver supports it and the converter bridges it otherwise. The format table itself
  is unchanged.
- **Note for users.** A format change reaches an application only when it reopens its audio
  stream. Chromium-based browsers (Brave, for example) keep their stream open while paused,
  so close and reopen such an application after changing the format.

### Sink delay reports were ignored without negotiation

- **Symptom.** The end-to-end latency estimate for the MR5's LDAC stream used the 200 ms
  LDAC default (209 ms in total) although the receiver reports its real delay.
- **Cause.** A DELAYREPORT was used only when delay reporting had been negotiated for the
  stream. The MR5's LDAC SEP does not offer the delay-reporting capability, yet it sends
  delay reports (about 364..396 ms).
- **Fix** (`transport_policy.h` `PnSinkDelayUs`, `device.cpp`). A report is used whenever
  the receiver sends one for the current stream: the acceptor stores a report only for the
  stream's own SEID and clears it at every configuration, so a stored report belongs to the
  running stream. The per-codec default applies only when no report has arrived.
  `SinkReportedDelayUs` now carries such a report too. `core_test` lane T (T6) checks a
  report used without negotiation (364 ms on LDAC).
- **Labels.** The panel's Status page shows "N ms reported by the headset" or "not
  reported; a per-codec default is assumed". `pna2dpctl status` adds "(reported without
  delay-reporting negotiation)" or "(not reported; a per-codec default is assumed)". On the
  MR5 the estimate went from 209 ms (the default guess) to about 401 ms (the real delay).

## Hardware results (14 October 2026)

Windows 11, Qualcomm FastConnect 7800, EDIFIER MR5 (LDAC), one PC.

| Driver | Observed result |
|---|---|
| 0.1.0.22 | LDAC HQ 990 kbps at 48 kHz, native (no conversion), 0 drops. The 44.1 kHz Default Format could not take effect (the first fix above), and the latency estimate used the 200 ms LDAC default (the second fix) |
| 0.1.0.23 | Windows' engine opened at 44.1 kHz as selected; the MR5 negotiated **LDAC at 44.1 kHz natively** (909 kbps). A 12-second WASAPI test stream drained fully at its end |
| 0.1.0.23, playing (LDAC 44.1 kHz) | Delay report of 396 ms used; end-to-end estimate about 401 ms |
| 0.1.0.23, Default Format switched to 24-bit, 48000 Hz during playback | LDAC stayed at 44.1 kHz with the converter bridging 48 to 44.1 kHz (status "44100 Hz (Windows 48000 Hz, converted)"); no reconnect or renegotiation; **the owner heard no crackle** |
| 0.1.0.23, Music profile applied while playing (`pna2dpctl set -d 1 latency.profile=music`) | Applied live without interruption |
| 0.1.0.23, whole session | 0 drops, 0 write failures, 0 latency trims, 0 ring overflows |
| Install of 0.1.0.23 over 0.1.0.22 | No reboot needed (audio stopped first) |

These are the driver's status and counters plus the owner's listening. The record does not
say whether they were read in the panel or with `pna2dpctl`, so the panel's Transport
selector and preset list stay on the [open list](#hardware-checks-still-to-do).

Not tested on hardware: the Sony CMT-SBT40D on 0.1.0.22 or 0.1.0.23 (switched off); SBC,
AAC and aptX through the converter; 96 / 88.2 kHz Windows formats; the 48 to 44.1 kHz
switch during playback; pause and resume on a converted stream; the auto-start reconnect
case; interference sessions with any profile; the Low latency profile's behaviour; an
upgrade from driver 0.1.0.21; the new-device defaults on a freshly paired receiver; long
sessions; sleep / wake; Driver Verifier and HLK; Secure Boot or Memory Integrity (HVCI) on. See
[hardware checks still to do](#hardware-checks-still-to-do).

## Release verification (driver 0.1.0.23 / application 0.1.0.36)

Software, for the hardware-found fixes on top of the second review's build (nothing here
was measured on a receiver beyond the results above):

- `core_test` **58,106** checks, 0 failures.
- Quality harness **1,252** jobs and 24 FFmpeg baselines (1,276 in all), including the 81
  alias-free transition jobs; **0** defect flags, 0 global flags (known issue #31 within its
  bounds); all **905** native-rate jobs bit-identical to the 0.1.0.21 baseline.
- Final release run (14 October 2026): `verify.ps1 -Sign -Sanitize -Interop` passed all
  **34** software checks: the signed driver package, InfVerif, ApiValidator, the
  AddressSanitizer builds of the core, SBC, control, service, process-wait and audio-health
  tests, the independent SBC and plugin decoders, the installer payload and the panel's
  start-up. Hardware playback is outside that script.
- Control panel: the Status page's Windows rate row (the first 0.1.0.36 build cut off the
  end of the combined format line) was checked against the running panel with
  `build/check-panel-text-fit.ps1`: 90 texts, none cut off at the current or the minimum
  window size (3 cut off on the first build).

## Versions

- Driver 0.1.0.23: `PNA2DP_DRIVER_VERSION 0x00010017`, `pna2dp.rc` 0,1,0,23, INF
  `DriverVer = 10/14/2026,0.1.0.23`. Driver 0.1.0.22 (`0x00010016`, 10/13/2026) was an
  intermediate build, tested on hardware but never published.
- Application 0.1.0.36: `tools/pna2dp-panel/version.h`. Application 0.1.0.35 was an
  intermediate build, never published.
- ABI 1, `PNA2DP_CONFIG` and the codec-host protocol (`SampleRate` is the codec rate) are
  unchanged. `PNA2DP_STATUS.Reserved[4]` became `EngineSampleRate` and `Reserved[3]`: same
  size (224 bytes) and offsets, older drivers report 0 (`pnctl_test` asserts the layout).
  New flag `PNA2DP_STATUS_FLAG_RATE_CONVERTED` (0x1000).

## Verification

On the development PC, 13 October 2026; nothing installed, signed or loaded, and no
receiver used. The figures for #1 are those of the first converter design (see
[testing](06-testing-plan.md)); the alias-free redesign's run is recorded
[above](#alias-free-band-plan-14-october-2026). The list below records the #2 / #5 step
first.

- Red before green: `pnctl_test` failed 6 checks against the 0.1.0.34 tools (defaults,
  automatic codecs, Best quality preset, preset name); `core_test` did not compile
  against the 0.1.0.21 policy (`SoftGateUs`, `PnAbrSoftGateUs`, `PnSbcPackingTargetMs`);
  the harness's Low latency jobs flagged the trim defect (3 CLICK(pre-codec) flags each)
  before the `PumpTrim` fix.
- `core_test`: 57,297 checks, 0 failures (defaults and validation, the save rule, the
  profile mapping, the gate rules, the link simulations, packing, the double-trim
  crossfade). `pnctl_test` passes (profile text round trip, presets accepted unchanged by
  the driver's own validation). `sbc_test`, `plugin_test`, `health_test`,
  `secure_path_test`, `service_failure_test`, `process_wait_test`, `boot_mode_test`,
  `test-interop.py` and `test-plugin-interop.py` pass.
- Quality harness: **983** jobs, **0** defect flags, plus known issue #31 within its
  bounds. Every job that existed before (976 distinct ids) reports metrics identical to a
  baseline run of the unmodified tree (Balanced and every native path unchanged). New
  jobs: Music with the 330 ms stall (0 trims), Music with a 520 ms stall (exactly one
  crossfaded trim), Low latency with the 330 ms stall (3 trims, crossfade clean), for the
  Sony SBC dual 53 path and LDAC HQ; the Balanced fault-trim jobs must trim exactly once.
- Driver Release build with `/WX`, InfVerif and ApiValidator; `/analyze` at its baseline
  of 23 warnings; control panel, `pna2dpctl` and tests build with no warnings.

#1, the sample-rate converter:

- Baselines before any converter code: the native conversion golden of `core_test` from the
  0.1.0.21 `pcm.cpp`, and the harness's hashes of every conversion block and packet of all
  959 jobs (`tools/audio-test/native-baseline-0.1.0.21.json`, `--hashes-out`).
- Red before green: 28 converter checks failed with the plan restricted to 0.1.0.21's
  pairs, 15 policy checks with `PN_RATECONV_ANY` reduced to the 0.1.0.21 rules, 12 checks
  of the input-side fade-in against an output ramp; the harness flagged the converted aptX
  HD restart (+28 dB) before that fix; the pause-drain simulation shows 48 cases for the
  single extra block of 0.1.0.21.
- `core_test` 57,577 checks, 0 failures; `pnctl_test`, `sbc_test`, `plugin_test`,
  `health_test`, `secure_path_test`, `service_failure_test`, `process_wait_test`,
  `boot_mode_test` pass; `test-interop.py` 19 / 0 and `test-plugin-interop.py` 28 / 0.
- Quality harness: 1139 jobs and 24 FFmpeg baselines, 0 defect flags (known issue #31
  within its bounds). All 905 native-rate jobs are bit-identical to the 0.1.0.21 baseline
  (`--hashes-check`, on by default in `build/test-audio-quality.py`), and their metrics
  equal the run before the change; only the 53 jobs of the four 2:1 configurations changed,
  as intended. 181 new jobs: the rate group, and fault and pause-race jobs through the
  converter.
- All targets build with no warnings: driver Release and Debug (`/WX`, InfVerif,
  ApiValidator), `/analyze` at 23, host, plugins, plugin tests, tests, CLI, panel, harness.

## Hardware checks still to do

Done on 14 October 2026 (see [hardware results](#hardware-results-14-october-2026)): the
MR5 at a 48 kHz and a 44.1 kHz Windows format (LDAC native at each), and a switch from
44.1 to 48 kHz during playback (bridged, no renegotiation, no crackle heard). Still open:

- A freshly paired receiver (or the **Driver defaults** preset) starts at LDAC HQ; a device
  with an older saved SQ ceiling keeps it until changed (the MR5's saved configuration was
  already HQ in this session, so it cannot show the second case as is).
- A 2.4 GHz interference session with Music and with Balanced: count trims and ABR
  downgrades (`pna2dpctl stats`, Quality > Events). The Low latency profile on a receiver.
- The Transport tab's selector and the widened preset list on a real display.
- EDIFIER MR5: the switch in the other direction (48 to 44.1 kHz during playback), and a
  pause and resume on a converted stream ending and starting the sound cleanly. With
  **auto start on connect**, a reconnect configures LDAC 48 kHz before audio opens; a first
  playback at 44.1 kHz must reconfigure to native 44.1 kHz before the sound starts.
- SBC, AAC and aptX through the converter; 96 and 88.2 kHz Windows formats.
- Sony CMT-SBT40D at 48 kHz: the native path is unchanged (SBC dual channel, bitpool 53,
  672 kbps, no `rate converted` flag), also when the receiver starts the connection and
  offers 44.1 kHz joint stereo: it is reconfigured before playback to 48 kHz dual channel,
  as with 0.1.0.21 (docs/06).
- Long sessions, sleep / wake, Driver Verifier and HLK, and Secure Boot / Memory Integrity
  (HVCI) on (which needs a production-signed driver).
