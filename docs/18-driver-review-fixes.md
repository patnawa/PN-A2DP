# Driver review fixes: driver 0.1.0.21 / application 0.1.0.34

Record for the [v0.1.0.34 prerelease](releases/v0.1.0.34.md): application, control
panel and installer **0.1.0.34**, custom driver **0.1.0.21**. It covers everything
since v0.1.0.30 (application 0.1.0.30, driver 0.1.0.18).

Status on 13 October 2026: **software-verified, with partial hardware validation**.
The full verification run passed (34 checks). On hardware, the Sony CMT-SBT40D (SBC)
was tested on driver 0.1.0.19 and the EDIFIER MR5 (LDAC) on driver 0.1.0.20; driver
0.1.0.21 was installed and loaded on the development PC, but its playback was not
re-measured, and the Sony was not re-tested after 0.1.0.19. See
[hardware results](#hardware-results-2026-10-13) and
[remaining verification](#remaining-verification).

## Summary

- A full review of the driver, codec host and build produced about 40 findings:
  connection handling, codec host robustness, audio timing and quality, the Windows
  audio endpoint, security and build. All were implemented. Where a decision could be
  separated from kernel code it now lives in a pure header that `core_test` builds in
  user mode (`transport_policy.h`, `sdp_policy.h`, `audio_policy.h`, `wave_math.h`,
  `wave_formats.h`, `control_access.h`, `common/recovery_policy.h`), and a failing test
  was written before each fix.
- Five adversarial review rounds of the result confirmed **39** further defects
  (12 + 8 + 6 + 7 + 6), all fixed, and rejected 4 reports with reasons.
- An audio-quality phase (six analysts plus a new end-to-end measurement harness)
  reported 39 issues; 34 were confirmed, 31 fixed and 3 deliberately left unchanged with
  measured reasons. A review of those fixes found 11 more issues: 10 fixed, 1 kept as a
  deliberate trade-off. See [audio-quality phase](#audio-quality-phase-13-october).
- The control panel gained a new tab order, opens on the receiver in use, shows the
  SBC XQ option and has **Connect** / **Disconnect** buttons. Driver 0.1.0.21 answers
  Windows' own Connect / Disconnect requests, which earlier PN drivers rejected. A
  review of that feature confirmed 4 issues, all fixed.
- Versions: driver **0.1.0.21** (`PNA2DP_DRIVER_VERSION` 0x00010015, INF
  `DriverVer 10/13/2026,0.1.0.21`); application, control panel and installer
  **0.1.0.34**. Intermediate builds: driver 0.1.0.19 (review fixes, application
  0.1.0.31) and 0.1.0.20 (audio-quality phase). Application 0.1.0.31-0.1.0.33 and
  drivers 0.1.0.19-0.1.0.20 were development builds and were not published.
- ABI 1 is unchanged. Additions are additive only: status flags
  `PNA2DP_STATUS_FLAG_REMOTE_SUSPENDED` (0x200), `_UNREACHABLE` (0x400) and
  `_HALF_RATE` (0x800); configuration flag `PNA2DP_CFG_SBC_XQ` (0x400); and four
  statistics in the former `PNA2DP_STATS.Reserved[4]` words (size and offsets
  unchanged; older drivers report 0).
- `build/verify.ps1 -Sign -Sanitize -Interop` passed all **34** checks. Core checks
  grew from 26,199 to **57,229**. See [verification evidence](#verification-evidence-2026-10-13).

## What changed

### Connection and AVDTP interoperability

- **Locking.** Audio-engine, KS property and IOCTL threads no longer take the device
  state mutex or wait for Bluetooth I/O. They record an intent under a short
  `IntentMutex` and queue the worker. A stream close returns at once and the worker
  sends SUSPEND; a renegotiating `SET_CONFIG` and `IOCTL_PNA2DP_RECONNECT` complete
  asynchronously. The AVDTP lock is split into a command lock (one outgoing procedure
  at a time) and a short stream lock, so the acceptor answers the receiver while our
  command is outstanding. The lock order is documented in `device.h` and `avdtp.h`.
- **Collisions.** An answer to our command is applied only after every receiver
  command that arrived before it (bounded waits of 1 s). START from both sides counts
  as reached. A receiver SET_CONFIGURATION or OPEN during ours is refused (SEP_IN_USE,
  BAD_STATE). OPEN rejected with BAD_STATE is a collision only when a receiver OPEN
  was actually refused; a second consecutive collision aborts. A late answer is not
  applied to a stream the receiver has changed since. A media channel the receiver
  opened is adopted instead of opened twice. Receiver commands whose answer depends
  on the stream state wait (up to 1 s) while our media channel connects or disconnects.
- **Receiver SUSPEND is respected.** No START for 8 s unless playback restarts on the
  PC or the receiver STARTs, and at most one timed START per suspend episode. Audio
  that resumes after at least 1 s of silence, while the receiver keeps the stream
  suspended, counts as a local restart. 0.1.0.18 sent START again immediately.
- **Receiver-configured streams.** After the receiver's SET_CONFIGURATION the driver
  waits up to 1.5 s for the receiver's own OPEN before sending its own, then
  RECONFIGUREs to the engine rate if needed instead of tearing down. A codec the
  receiver chose is upgraded at most once per session, before START or at the next
  pause, never while playing.
- **SEP choice.** Receiver SEPs marked in use are tried last instead of skipped; our own
  configured SEP counts as free. SEP_IN_USE triggers one fresh DISCOVER. Each refused
  (codec, SEID) pair is skipped for the session and the next codec is tried, at most 8
  attempts; a refused delay-reporting request is retried once without it.
- **Failures and timeouts.** Every failed setup arms a retry: 1.5 s, doubling, capped at
  30 s, reset after 30 s of continuous streaming. The first transaction timeout ABORTs
  the stream, the second consecutive one disconnects. The AVDTP response timeout is
  3 s (was 4 s), the upper end of GAVDP's TGAVDP100 range. Closing a Configured or
  Opening stream now sends ABORT instead of dropping it silently.
- **Media stalls and link loss.** When media writes stop completing for 2 s, the worker
  probes the receiver with GET_CAPABILITIES while the pump keeps running. Answered and
  flowing again: the stream is kept (as 0.1.0.18 behaved for a 2-5 s radio fade).
  Answered but still stuck: ABORT. Unanswered: the session is dropped and the receiver
  marked unreachable. An HCI ACL-down event for the receiver closes the session. After
  an unanswered stall with the ACL still up, the connect worker keeps retrying
  signalling with back-off capped at 30 s. The ACL state is read at device start
  instead of assumed down.
- **Discovery and SDP.** A GET_ALL_CAPABILITIES failure falls back to GET_CAPABILITIES for
  that SEP; the receiver is treated as AVDTP 1.2 only on evidence. Discovery needs at
  least one SEP with valid capabilities. SDP is also queried for receiver-initiated
  sessions and retried in later sessions (at most 3 failures per device). The AVDTP
  version now comes from the SDP ProtocolDescriptorList, so `PNA2DP_STATUS.AvdtpVersion`
  is the AVDTP protocol version; `PNA2DP_CAPABILITIES.SdpProfileVersion` stays the A2DP
  profile version.
- **Strict acceptance.** Advertised capabilities are parsed leniently (repeated content
  protection entries, unknown categories). A receiver SET_CONFIGURATION that requests
  content protection is refused with INVALID_CP_TYPE, an unsupported category with
  BAD_SERV_CATEGORY, and a missing Media Transport with its own error. RECONFIGURE of a
  non-reconfigurable category is refused with INVALID_CAPABILITIES. DELAYREPORT is
  checked against our SEID and the stream state, and the stored delay resets with each
  new configuration. SCMS-T is not implemented.
- **Local SEP advertisement.** DISCOVER reports a local codec SEP in use when that codec
  is disabled or no encoder is registered for it. SBC is always available.
- **Power.** An endpoint power-down while the system stays in S0 SUSPENDs and keeps
  signalling up; real system sleep keeps the full stop. On resume the session is reset
  only if signalling or the ACL is actually gone.
- **Media MTU and write completion.** The MTU requested when opening or accepting the
  media channel is capped to one baseband packet of the local radio: 1017 bytes for
  3 Mb/s EDR or an unknown radio, 675 for 2 Mb/s only, 1013 (three DH5) for Basic Rate
  only. The pump uses min(receiver MTU, cap), except that a 2 Mb/s-only radio keeps the
  receiver's MTU and leaves the choice to SBC packing. The Sony on this PC is unchanged:
  min(895, 1017) = 895. A completed write now frees its slot before waking the pump.

### Codec host robustness

- **Quarantine with automatic recovery** replaces the permanent failure flag. Four
  consecutive host failures quarantine the host for 30 s, then 2 min, then 10 min;
  5 minutes of healthy results reset the escalation. A quarantined host counts as
  absent (kernel SBC fallback), and the quarantine ends by time without
  re-registration. A stream that fell back to SBC returns to the better codec at the
  next START or pause, once per session, never mid-stream.
- **Exact block accounting.** The driver takes a pended `GET_WORK` request before PCM
  leaves the ring and records each outstanding block (sequence, frames, submit time).
  A result for a later block writes off the earlier ones; duplicate and written-off
  results are ignored. The watchdog writes off only overdue blocks,
  oldest first, and skips their media time so the receiver sees a gap rather than a
  time shift. Each loss counts in `HostTimeouts` and `HostDroppedBlocks`.
- **Result validation.** Payload size, frame count and the media payload header are
  bounded; an invalid result is one dropped block and `STATUS_INVALID_PARAMETER` for
  the host. Claimed PCM consumption is capped to what the host actually holds.
- **Host process.** The encoder thread registers with MMCSS "Pro Audio" (falling back to
  time-critical priority). A rejected quality change keeps the old quality and still
  encodes the block. Result buffers are preallocated.
- **AAC** sets the encoder's peak bitrate so every access unit fits one media packet:
  about 300 kbps at 44.1 kHz with the Sony-sized MTU of 895, 320 kbps at 48 kHz. The AAC
  ABR rungs are now the ladder steps strictly below that ceiling.
- **LDAC**: libldac's unused internal ABR was removed; the driver's EQMID adaptation is
  the only one. A full 4-packet queue now drops the newest packet and keeps its PCM time.
- Quarantine and fallback are written to the System event log (see below).

### Audio quality and timing

- **WaveRT clock.** The frames due at each timer tick are computed with an exact
  remainder. A one-hour simulation shows zero drift at 44.1-96 kHz; the old rounding
  lost up to about 178 ms per hour (88.2 kHz, jittered ticks). The timer runs every
  2 ms when the notification interval is 5 ms or more, otherwise every 1 ms.
- **Unwritten packets.** A packet the engine has not written by the time playback reaches
  it is sent as silence instead of stale buffer contents, and counted in
  `WaveStarvedPackets`. After 8 consecutive starved packets the check pauses, in case an
  engine numbers its packets differently, and re-arms after 50 consecutive packets
  written on time.
- **Latency.** Pipeline latency is counted frame by frame: ring, codec host, queued
  results and L2CAP writes in flight, plus one write's measured send time (and the
  decimator's group delay). `MaxLatencyMs` is limited to 500 ms because the ring holds
  600 ms; the trim target stays at least 20 ms below it.
- **Volume ramps and trim crossfade.** Volume and mute changes ramp linearly across one
  encoder block (one packet's audio, about 8 ms for the Sony's SBC packets) instead of
  stepping. A latency trim crossfades over 4 ms from the
  discarded audio into the kept audio. Both apply to kernel SBC and host codecs.
- **24-bit SBC input.** The kernel SBC encoder now takes 32-bit input and keeps 24 bits,
  so software volume below 100 % no longer rounds to 16 bits first. 16-bit input at
  unity gain produces byte-identical frames.
- **Dither.** Host codecs fed 16-bit PCM (AAC) get deterministic TPDF dither of ±1 LSB
  when a sample is not already a 16-bit value (gain below unity, a wider source,
  decimation, a crossfade). Never when muted and never in a block of digital silence.
- **2:1 decimation.** A 96 or 88.2 kHz Windows format can feed a receiver at exactly
  48 or 44.1 kHz through a fixed-point 103-tap half-band filter (ripple 0.0009 dB to
  0.45 fs_out, stopband -80.1 dB from 0.55 fs_out). Other rate mismatches are still not
  converted. The status shows `HALF_RATE` while it runs.
- **SBC packing.** Frames per packet also consider the local radio's largest baseband
  packet when that costs fewer air slots per frame than filling the MTU. The Sony's
  dual-channel stream stays at 3 frames (685 bytes) per packet on a 3 Mb/s EDR adapter
  or with unknown radio data.
- **SBC XQ, default off.** `PNA2DP_CFG_SBC_XQ` selects dual channel, 16 blocks and
  8 subbands at the bitpool plain dual channel gets for the same receiver, MTU and radio
  (bitpool 53 = 617 / 672 kbps on the Sony), packed to the EDR packet both ends take
  (3-DH5 needs the receiver to report 3 Mb/s EDR, queried with
  `IOCTL_BTH_GET_RADIO_INFO`; 2-DH5 otherwise). The first version fixed bitpool 47 or 38,
  which gave the Sony 600 or 492 instead of 672 kbps (see the
  [audio-quality phase](#audio-quality-phase-13-october) below).
- **ABR** now separates link congestion from codec-host starvation, measures each write's
  own send time and can cut several rungs at once. See
  [automatic adapter quality](10-adapter-quality.md).
- **Fewer wakeups.** A WaveRT write wakes the pump only when it completes an encoder
  block, not on every write (46-47 wakes per second instead of 1000 in the test case of
  1 ms writes and 1024-frame blocks). A 4 ms safety tick remains.
- The F32 output conversion no longer uses floating point in kernel code.

### Audio-quality phase (13 October)

Six analysts reviewed the audio path (among the lenses: PCM and gain, AAC, aptX and
LDAC, timing and flow, stream lifecycle) next to a new measurement harness. Together
they reported 39 issues; two verifiers checked each one. 34 were confirmed: 31 are fixed,
3 were deliberately left unchanged (below) and aptX's high-frequency overload was
judged codec-inherent (FFmpeg's own aptX encoder gives the same clip counts). These
changes are in driver 0.1.0.20 and later.

The harness (`tools/audio-test/quality_test.cpp`, `build.ps1 -Target
quality-tests`, run by `verify.ps1 -Interop` through `build/test-audio-quality.py`)
drives the production conversion, codec selection, SBC packing, RTP writer, kernel SBC
encoder and the codec plugins, decodes with FFmpeg and flags clicks, gaps, clipping and
accounting errors. Kernel-only parts (the PCM ring, WaveRT hold-back and the pump's
stream finish) are replicated in the harness, not run from the kernel code. Fixed:

- **SBC XQ** streams at plain dual channel's bitpool (above); never below it.
- **SBC packing follows the bitpool.** Frames per packet are recomputed at every ABR rung
  and live bitpool change. A bitpool lowered live and kept across a pause used to size the
  packets for the low bitpool; the next ABR step or raise sent packets over the MTU, all
  dropped (total silence). Lower rungs now also send fewer packets: on the Sony, bitpool 26
  costs 321 instead of 500 baseband slots/s. A lowered bitpool stays the ABR's top rung.
- **aptX packets** carry 4-byte (aptX HD: 6-byte) codewords and fill the MTU in multiples
  of 8 codewords (the parity sync period) up to a 160-codeword cap (13.3 ms at 48 kHz):
  at MTU 895, aptX 160 codewords (the cap) and aptX HD 144, instead of about 110 and 73.
  aptX LL keeps packets of at most 112 codewords (9.3 ms), as on MTU 895 before.
- **AAC:** ABR steps below the configured VBR quality are CBR at the rung's bitrate (VBR
  ignored the bitrate, so the first step changed nothing); CBR at 96 kbps per channel and
  more codes up to 19-20 kHz (FDK's table cut it at 17 kHz); VBR access units stay within
  the bitrate the receiver was configured with.
- **Codec host stalls:** one stall of the host process counts as one failure (plus one per
  500 ms it lasts), not four, so a 220-290 ms hiccup no longer quarantines the host and
  renegotiates to SBC.
- **LDAC** is refused for the session when the media MTU is below 679 bytes, instead of
  failing every encode until the host is quarantined.
- **Starts, gaps and pauses:** the WaveRT clock holds for a pending stream start; starved
  packets, restarts, pauses and resumes fade (see [volume and audio quality](08-volume-and-audio-quality.md)).
  A late render tick no longer silences a packet the engine was never asked to write, and
  the starvation check re-arms once the engine writes on time again.
- **Absolute volume hand-off:** the receiver's held level is not applied again in software.
- **PCM headroom:** float overs and decimator overshoot survive a volume below 100 %; Inf
  input is no longer a full-scale pop (the fix review below conceals it).
- The in-tree SBC test decoder kept its synthesis state across bitpool changes (it reset it,
  which made every ABR change look like a full-scale click in tests).

Deliberately not changed, with the measured reason:

- **No limiter or pre-encoder headroom (#13).** Programme at 0 dBFS can still clip in the
  receiver's decoder. On realistic masters the verifiers measured that error 24-43 dB
  below the codec's own noise; a fixed -1 to -3 dB would cost loudness on every track,
  and a true-peak limiter needs look-ahead in fixed-point kernel code. A user setting
  would need a new configuration field, so it is deferred.
- **AAC at 88.2/96 kHz (#14).** AAC is still negotiated at those rates when the engine
  runs there, at the same audio bandwidth and with more air time. It is reachable only
  with the opt-in `EXPOSE_ALL_RATES` flag and a 96 kHz AAC receiver; a blanket half-rate
  rule made small-MTU receivers 3-5 dB worse.
- **Dual-channel ABR floor (#31).** Under heavy congestion the dual-channel SBC ladder can
  reach bitpool 8 (about 132 kbps), which is audibly distorted. A higher floor turned codec
  noise into latency trims and gaps on a link that sustains only 150-200 kbps; the real
  fix (reconfiguring to joint stereo mid-stream) is a state-machine change with an audible
  gap that could not be tested. The harness reports it as a bounded known issue (below).

Fix review (11 issues found in the changes above; 10 fixed, 1 kept as a trade-off),
measured with the end-to-end harness `build/test-audio-quality.py`:

- **Pause without a click, whatever the timing (#1/#7).** At a KS pause the WaveRT stream
  handed its 2 ms hold-back FIFO to the pump unfaded; that write woke the pump thread, which
  could encode the newest audio at full level before the fade ran, and with an empty ring the
  pause got no fade, no silence, no encoder drain and no fade-in on resume. The FIFO, the 4 ms
  fade and the silence padding now happen under one ring-lock hold (`PumpFinishStream` takes
  the tail), an empty ring is padded too, and the engine's end of stream ends the sound the
  same way. Harness, worst-case race at 16 pause instants on 7 codecs: before, 11 of 112
  pauses were hard cuts (-10.8 to -11.2 dB re tone; AAC's fade shrank to -42.2 dB); after,
  every pause fades (worst -48.3 dB re tone).
- **The dual-channel ABR floor is reported, not hidden (#2).** The harness had relabelled the
  reachable dual-channel floor rung (bitpool 8, 132 kbps) as a synthetic stress configuration.
  It is now a named known issue (#31, unchanged by design) whose verdicts are listed, and the
  run fails if it measures worse than recorded (THD+N -48.4 dB at -6 dBFS, pink SNR 8.4 dB,
  HF burst +15.6 dB, which FFmpeg's SBC encoder at bitpool 8 reproduces).
- **Receiver-held volume ends with the link (#3/#8).** The level the receiver keeps after
  absolute volume detaches applies only while the ACL stays up (the Sony's AVDTP restarts).
  A link loss, or a detach after it, falls back to the user's volume until Windows sends
  absolute volume again: quieter, never louder than the slider.
- **Resumed audio keeps its attack (#4).** A stream start fades in only when the discarded PCM
  ended in a signal; the engine's silence while the receiver kept the stream suspended no
  longer softens the first 8-21 ms of the music that follows.
- **Fades keep float overs (#5/#9).** `PnPcmRamp` works in 64 bits and writes float above full
  scale back unclipped, so a fade edge in hot float programme at reduced volume is no step
  (+6 dBFS at -12 dB: the first faded frame was 6 dB low).
- **NaN and Inf (#6).** A non-finite float sample repeats the channel's last finite sample; a
  run fades linearly to silence over 32 samples. Harness NaN/Inf job: SNR 50.9 -> 57.5 dB (AAC),
  52.8 -> 59.6 dB (aptX HD).
- **aptX LL packet size (#10)** is unchanged (112 codewords, 9.3 ms at 48 kHz); the comment now
  states the trade: up to 2.3 ms more than the old half-filled packets below an 895-byte MTU, in
  fewer packets.
- **LDAC on a small media MTU (#11)** is refused right after OPEN, before START, and the next
  codec is configured in the same pass, instead of after a START, a CLOSE and the 1.5 s setup
  back-off on every connection.

### Windows audio endpoint

- **Formats.** The format table is built by a tested pure function. 16-bit stereo at
  48 and 44.1 kHz is always offered, 48 kHz first. When a codec with more than 16-bit
  input (aptX family, LDAC) is negotiated or appears in the receiver's cached
  capabilities, each rate offers 24-in-32 first, then 32-bit float, then 16-bit.
  `EXPOSE_ALL_RATES` adds every rate an enabled codec supports (at most 18 entries).
  IEEE float formats are accepted. The table follows the receiver: Windows gets
  `KSEVENT_PINCAPS_FORMATCHANGE` only when it changes, and the jack description now
  advertises dynamic format change. An SBC-only receiver such as the Sony keeps exactly
  [48 kHz/16, 44.1 kHz/16]. The float pin range is fixed at driver start, so a hi-res
  receiver gets float formats only after its capabilities are cached and the driver
  restarts.
- **Jack.** The jack state is per device, and changes raise
  `KSEVENT_PINCAPS_JACKINFOCHANGE`. The endpoint starts connected. Only a user
  disconnect unplugs it; any sign of the receiver (ACL up, signalling up) plugs it
  again. Link loss and unanswered stalls keep it plugged so that the next playback pages
  the receiver. Review rounds 2 and 5 removed earlier unplug triggers that could leave
  the endpoint stuck unplugged or make Windows switch outputs twice.
- **One-shot connect and disconnect.** `KSPROPSETID_BtAudio` on the topology filter:
  `KSPROPERTY_ONESHOT_RECONNECT` requests a connection without tearing down a working
  session and lifts a user disconnect. `KSPROPERTY_ONESHOT_DISCONNECT` stops the pump,
  drops the session, unplugs the jack and connects nothing until a reconnect request, a
  new local playback start or an incoming connection from the receiver. Both are
  documented as GET with no value, which is what Windows' own Connect / Disconnect and
  the panel and CLI send; SET is still accepted (driver 0.1.0.21).
  `IOCTL_PNA2DP_RECONNECT` (panel and CLI) is still a disconnect-first reset.
- **Presentation position.** Interpolated from the last timer tick to the query time,
  minus a latency (pipeline latency plus sink delay) that follows changes at no more
  than 1 % of real time. It never decreases within a run and snaps, rather than slews,
  at a pump start or stop. The sink delay is the receiver's delay report when delay
  reporting was negotiated and a report arrived; otherwise a per-codec heuristic
  default (SBC 150 ms, AAC 180, aptX 150, aptX HD 180, aptX LL 60, LDAC 200).
  `GetHWLatency` still reports zero, because this software endpoint has no DMA FIFO.
- **Volume and lifetime.** KS volume and mute accept only the master channel, and volume
  snaps to the advertised 0.5 dB steps. The adapter holds references on its miniports,
  so volume events cannot reach a freed miniport. An allocated WaveRT buffer smaller
  than requested is rejected.

### Security and observability

The private control interface now records a role per handle from the caller's token
when the handle is opened, and checks every IOCTL against it (in addition to the
handle's read/write access):

| Role | Determined by | IOCTLs |
|---|---|---|
| User | any other token | `GET_VERSION`, `GET_STATUS`, `GET_CAPABILITIES`, `GET_CONFIG`, `GET_STATS`, `GET_QUALITY`, `WAIT_EVENT`, `SET_CONFIG`, `RECONNECT`, `RESET_STATS` |
| Admin | BUILTIN\Administrators enabled, not deny-only (an elevated token) | the above plus `GET_TRACE` and `SBC_SAMPLE` |
| System | token user is LocalSystem (S-1-5-18) | the above plus `CODEC_REGISTER`, `CODEC_UNREGISTER`, `CODEC_GET_WORK`, `CODEC_PUT_RESULT`, and any unknown code |

`CODEC_REGISTER.HostPid` must be the calling process. What this means for users:

- The control panel is unaffected; it uses only User-role requests and still runs
  unelevated.
- `pna2dpctl trace` needs an elevated prompt; unelevated it prints a hint.
- `sbc_capture` needs an elevated prompt.
- `ks_test --device <n>` should be run both unelevated (it expects trace and capture to
  be denied) and elevated. It never tries to register a codec host as LocalSystem.
  Against driver 0.1.0.18 or older it prints SKIP for the role checks.
- Codec host console mode registers only when run as LocalSystem (for example with
  `psexec -s`). Run as an elevated administrator it is refused, and the driver stays on
  kernel SBC. The installed service runs as LocalSystem and is unaffected.
- Unknown or future IOCTL codes now fail with access denied for callers other than
  LocalSystem.

Other hardening: read, write, flush and other non-control requests on a control handle
are completed with `STATUS_INVALID_DEVICE_REQUEST` instead of being passed to KS, which
would interpret user-mode handle fields as its object header (defensive: the reviewers
disagreed on whether PortCls installs those handlers). `_purecall` now fast-fails, and
the sized `operator delete` is defined explicitly.

Observability:

- **TraceLogging provider** `PnA2dp.Driver`, GUID `{7015a1ac-0944-5322-6528-aac0e90c3960}`
  (the standard hash of the name, so tools can enable it by name), ETW levels 2-5.
- **`DebugLevel`** now defaults to 2 (warnings), in the INF and in code. It controls
  debugger output and whether verbose messages enter the trace ring.
- **Trace ring** (`pna2dpctl trace`) always keeps errors, warnings and information, even
  at `DebugLevel` 0 (before, 0 meant an empty ring); verbose messages only at 4. Debugger
  lines are built from the ring text and cut at 191 characters.
- **System event log** source `pna2dp`, using standard `IO_ERR_*` messages from
  `IoLogMsg.dll`: crash-guard refusal (`IO_ERR_CONFIGURATION_ERROR`, unique value =
  unclean start count), load and start failures (`IO_ERR_INTERNAL_ERROR`, unique value =
  the NTSTATUS), codec host quarantine and fallback (`IO_ERR_LAYERED_FAILURE`, unique
  value 0x0001nnnn = quarantine number, 0x0002nnnn = codec whose encoder vanished).
  The source is registered when the INF is installed.

### Build and certification readiness

- The driver compiles with `/W4 /WX` (was `/WX-`) and builds with zero warnings in
  Release and Debug; the shadowing warnings C4456/C4457 are no longer suppressed, the
  WDK header warning C4471 is. It adds `/Qspectre` (Spectre variant 1 load barriers) and
  links with `/CETCOMPAT` (shadow-stack compatible).
- Every driver build runs ApiValidator against the WDK's universal DDI list right after
  linking, and InfVerif `/w /v` on the stamped INF. Either failure fails the build;
  before, InfVerif ran only for `-Production`.
- `build/check-analyze.ps1` runs the Debug `-Analyze` driver build and fails when the
  number of unique warnings rises above `build/analyze-baseline.txt`. The baseline is
  **23**, all in WDK/KMDF headers; project sources have none. Policy: after removing
  warnings, lower the baseline with `-UpdateBaseline`; never raise it to hide a new
  one. Only the count is compared, so a fixed warning can mask a new one.
- `verify.ps1` gained the `driver-analysis` and `api-validator` checks.
  `build.ps1 -Target plugins` builds the codec plugins alone.
- `.github/workflows/verify.yml` builds the tests, driver, host, CLI, panel and plugins
  unsigned on `windows-2022`, runs InfVerif, ApiValidator and the pure unit tests, and
  runs the analysis ratchet as advisory (warning counts depend on the runner's MSVC).
  It never signs, installs or opens a device. **It has not been run on GitHub.**
- Not done: ARM64 is not built (no ARM64 toolset on this machine). A `[*.Wdf]`
  `KmdfService` section is deliberately omitted; the INF comment explains why (KMDF in
  miniport mode under PortCls, framework bound from the binary) and says to add it only
  after an install test. CodeQL has not been run. HLK has not been run.
- `build.ps1 -Target quality-tests` builds the audio-quality harness; `verify.ps1`
  builds and runs it only with `-Interop` (it needs Python, FFmpeg and the codec
  plugins). It fails on any defect flag, and on the known dual-channel floor issue (#31)
  measuring worse than recorded.

### Control panel and command line (application 0.1.0.31-0.1.0.34)

- **Tab order.** PN-A2DP devices: Status, Quality, Codecs, SBC, AAC, aptX, LDAC,
  Transport, Headset, Adapter, Driver. Windows mode: Audio, Quality, Driver. Each
  device opens on its first tab (Status, or Audio in Windows mode); before, every device
  opened on Quality.
- **Opens on the receiver in use.** With no earlier selection, the panel selects the
  receiver whose output is the Windows default, then one that is streaming, then one
  that is connected, then any PN-A2DP device.
- **"Connected" instead of "Discovering".** The driver reports
  `PNA2DP_STATE_DISCOVERING` whenever signalling is up without a configured stream,
  which is mostly a connected, idle receiver; discovery itself takes well under a second.
  The panel, `pna2dpctl` and the diagnostic export now call that state "Connected". The
  driver value is unchanged.
- **SBC XQ checkbox** on the SBC page and `sbc.xq=0|1` in `pna2dpctl set` and the
  configuration text. It overrides the channel mode, blocks and subbands shown above it.
  On receivers like the Sony and the MR5 (advertised bitpool 53) XQ gives bitpool 53,
  672 kbps at 48 kHz, the same as plain dual channel; it never streams below plain dual
  channel. The SBC XQ preset now sets it with the receiver's maximum bitpool.
- **Connect and Disconnect** under the device list, and `pna2dpctl connect|disconnect`
  (PN-A2DP devices) and `pna2dpctl windows connect|disconnect` (any paired receiver).
  They send the same `KSPROPSETID_BtAudio` one-shot GET that Windows Settings sends, on
  the topology filter behind the receiver's audio endpoint. A PN-A2DP receiver with no
  Windows audio output at all falls back to the driver's reconnect (a link reset), and
  only while the driver reports the session idle or failed. Details are in
  [control panel and installer](07-control-panel-and-installer.md).
- **Status values.** The Status page's flags field shows the new status flags
  ("suspended by receiver", "receiver unreachable", "2:1 rate conversion");
  `pna2dpctl stats` shows codec-host dropped blocks, PCM ring overflows, starved packets
  and AAC fragmented sends, and `pna2dpctl config` lists `sbc-xq`. See
  [audio diagnostics](14-audio-diagnostics.md).
- `pna2dpctl trace` prints a hint when it is denied for lack of elevation.

## User-visible behaviour changes

Compared with driver 0.1.0.18 / application 0.1.0.30. Unless a row says otherwise, a
change is covered by software tests only; what was observed on receivers is listed in
[hardware results](#hardware-results-2026-10-13).

| Area | Now |
|---|---|
| Receiver suspends the stream | Respected: no START for 8 s unless playback restarts or the receiver STARTs; one timed START per episode. 0.1.0.18 restarted at once. |
| Windows **Disconnect** / **Connect** for the device | Supported through the one-shot properties, as GET (driver 0.1.0.21; 0.1.0.18-0.1.0.20 accepted only SET, so Windows Settings' own buttons failed). Disconnect unplugs the endpoint until a reconnect, a new playback start or the receiver connecting. Connect leaves a working session alone. The panel and CLI have the same commands. Owner-tested on hardware, reported pass (no automated log). |
| Receiver switched off or out of range | Endpoint stays plugged (as before); the session is dropped after an unanswered stall probe and retried with back-off. A 2-5 s fade keeps the stream. |
| Receiver-initiated session | The driver waits up to 1.5 s for the receiver's OPEN; if the receiver waits for us instead, audio starts up to 1.5 s later. |
| AAC bitrate | Capped so each access unit fits one packet: about 300 kbps at 44.1 kHz with MTU 895. Lower on small MTUs. |
| SBC XQ | New SBC-page checkbox and `sbc.xq` setting, default off. Streams at plain dual channel's bitpool, never below it (672 kbps at 48 kHz on a bitpool-53 receiver). The SBC XQ preset sets it at the receiver's maximum bitpool, no override. Toggling it renegotiates live. Not yet re-checked on hardware after the 0.1.0.20 rule change. |
| AAC | CBR at 96 kbps per channel and more codes up to 19-20 kHz (was 17 kHz); ABR steps below the top are CBR at the rung's bitrate; VBR stays within the receiver's declared bitrate. |
| aptX family | Packets carry about twice the audio, up to the MTU or 160 codewords, in multiples of 8 codewords (at MTU 895: aptX 160, aptX HD 144); aptX LL keeps at most 112 codewords. |
| Pause, resume, gaps and starts | Fade out and in instead of cutting; the first sound after an idle suspend is delayed, not discarded. |
| LDAC on a media MTU below 679 bytes | Refused right after OPEN; the next codec is configured in the same pass. |
| Volume and mute | Ramp over one encoder block (about 8 ms on the Sony) instead of stepping; latency trims crossfade over 4 ms. |
| SBC at reduced volume | 24-bit precision into the encoder instead of 16-bit rounding. |
| AAC input | TPDF dither when the 16-bit input is not exact. |
| WaveRT timing | 2 ms timer for notification intervals of 5 ms or more; notifications can be up to 2 ms late. Unwritten packets play as silence. |
| ABR | A write is slow when its own send time exceeds its media time plus 10 ms (18 ms on the Sony path); 0.1.0.18 compared completion latency, queueing included, with twice the media time plus 10 ms (26 ms). Large cuts only when the TX queue was full; one urgent rung per stall episode; stalled writes excluded from the throughput estimate; recovery unchanged (30/10/5 s, 60 s penalty). SBC ladder gains a "fits 2-DH5" rung, so level numbers below the top shift by one. |
| Maximum buffer | Stored as at most 500 ms. The panel's Transport slider still allows 1000 ms; the driver clamps it on Apply. |
| Status values | `SinkReportedDelayUs` is the receiver's AVDTP delay report only, 0 when none was negotiated or received (driver 0.1.0.20; 0.1.0.19 filled in the per-codec default). `EstimatedLatencyUs` includes the per-codec default sink delay when no report exists; `MaxBitrateBps` shows the effective ceiling while streaming; `AvdtpVersion` is the AVDTP protocol version. The panel and CLI show `PNA2DP_STATE_DISCOVERING` as "Connected". |
| Panel | Tab order Status, Quality, Codecs, ... Driver (Windows mode: Audio, Quality, Driver); opens on the first tab and on the receiver in use; Connect / Disconnect buttons. |
| Diagnostics | `DebugLevel` default 2; the trace ring keeps information at every level. `pna2dpctl trace` and `sbc_capture` need elevation; codec host console mode needs LocalSystem. |
| Codec host failure | Quarantine (30 s / 2 min / 10 min) with SBC fallback and automatic return at the next start or pause. |

## Review process

**Implementation.** The roughly 40 review findings were implemented in four work lanes
(transport, audio endpoint, pump and codec host, security and build), each in two or
three steps, followed by an integration step that wired the lanes together. Each lane first extracted a pure seam, added characterization tests
of the existing behaviour (differential tests against verbatim copies of the old code
for codec choice, format checks, SDP parsing and the acceptor) and RED tests encoding
the corrected behaviour, then turned them green. A structural script, outside the repo,
checked that no audio-engine or IOCTL entry point takes the state mutex or waits for
Bluetooth I/O (12 violations at the start, 0 after).

**Adversarial review.** Five rounds, each with reviewers working through separate
lenses (concurrency, protocol, audio math, kernel, security/ABI, regression against
0.1.0.18) and each finding checked by two or three independent verifiers.

| Round | Reported | Confirmed | Rejected | Confirmed severity |
|---|---|---|---|---|
| 1 | 14 | 12 | 2 | 1 high, 1 medium, 10 low |
| 2 | 8 | 8 | 0 | 1 medium, 7 low |
| 3 | 7 | 6 | 1 | 3 medium, 3 low |
| 4 | 7 | 7 | 0 | 1 high, 3 medium, 3 low |
| 5 | 7 | 6 | 1 | 1 medium, 5 low |
| **Total** | **43** | **39** | **4** | 2 high, 9 medium, 28 low |

The two high findings were an ABR budget that counted a link stall as busy time (one
stall could drop SBC to the floor rung for about 100 s; round 1) and non-control
requests on a control handle reaching KS (round 4; fixed defensively). All 39 were
fixed. Every fix with a user-mode seam has a regression test. From round 2 on these
were shown failing against the old behaviour before passing (round 2: 695 failures,
round 3: 8, round 4: 170, round 5: 11). Round 1's tests were shown failing only at
compile time; their behavioural red was argued from the old code. Fixes without a seam
(token reading, PortCls and WDF lifetimes, IRP hooks, BthPort queries, the
AVDTP transaction interleavings) were checked by `/WX` builds, the analysis ratchet,
ApiValidator and code reading only.

Rejected, with reasons:

- *The caller role is taken from an identification-level impersonation token* (two
  reports in round 1; 1 of 5 verifier votes real). The verifiers judged the described
  attack blocked earlier, by the security of the device object PortCls creates, so the
  scenario cannot occur as described. The observation itself is correct: the role
  capture does not check the impersonation level. It is listed under open items.
- *The SDP query delays START of a receiver-initiated session* (round 3). The query
  runs when the receiver configures the stream, not when playback starts.
- *A quarantined but running codec host shows as "codec host missing"* (round 5). This
  is not a regression: it existed before and is milder now, and the user action the
  report warned about actually resolves it.

The loop was stopped after round 5, **before a round came back with no confirmed
findings**. The later rounds mostly found low and medium edge cases in the
interactions between the new mechanisms (stall probe versus a new signalling session,
OPEN collision ordering, suspend episodes versus continuous playback, jack transitions
during an outage, ABR stall accounting). Further defects of that kind are likely.

**Audio-quality phase.** Six analysts and the measurement harness reported 39 issues;
each was checked by two verifiers. 34 were confirmed: 31 fixed, 3 deliberately not fixed (#13, #14, #31,
see [above](#audio-quality-phase-13-october)); aptX's high-frequency overload is
codec-inherent. Five reports were rejected with reasons. A review of the
fixes found 11 issues: 10 fixed, and the aptX LL packet size (#10) kept as a trade-off
(up to 2.3 ms more packetisation below an 895-byte MTU, in fewer packets). Fixes in
kernel paths without a user-mode seam (the pause and end-of-stream finish under the
stream lock, the ACL-loss volume reset, the in-pass LDAC refusal) were compiled and
modelled in the harness, not run as kernel code in tests.

**Connect / Disconnect.** A review of the new feature confirmed 3 defects, all fixed:
a topology failure after the endpoint was found was reported as "no endpoint", which
could trigger the link-reset fallback and wrong "pair it again" advice; a button that
disabled itself left the dialog without keyboard focus; and the rejection text blamed the
PN-A2DP driver version for any driver. A fourth report asked for pairing and removal in
the panel; the owner chose Connect / Disconnect only, so pairing and removal stay in
Windows Settings > Bluetooth & devices (the status line and documentation point there).
One report was rejected (the size-probe status of the property set's basic support). The driver's verb rule is a pure function with a regression test that failed
against the old SET-only rule before the fix.

## Verification evidence (2026-10-13)

`build/verify.ps1 -Sign -Sanitize -Interop`, run on 13 October at 18:42 on the
development PC, passed all **34** checks. The machine-local report is not published.

| Check | Result |
|---|---|
| `driver-analysis` | 23 warnings, baseline 23, all in WDK/SDK headers |
| `api-validator` | "All binaries are Universal" |
| `inf-verification` | "INF is VALID" |
| `core`, `asan-core` | PASSED, **57,229** checks (26,199 on 12 October), also under AddressSanitizer; includes 50,000 deterministic malformed packet inputs |
| `sbc`, `asan-sbc` | `sbc_test` PASSED, including a bitpool-switch decode case |
| `sbc-independent-decoder` | FFmpeg decodes **19/19** SBC vectors, including 3 SBC XQ vectors and 3 encoded from 24-bit input |
| `plugins` | Plugin parameter matrix and AAC packet-fit cases pass; LDAC encodes at every quality level, rate and input format |
| `plugin-independent-decoder` | **28/28** AAC, aptX, aptX HD and aptX LL vectors |
| `build-audio-quality`, `audio-quality` (new) | **977** jobs, **0** defect flags, 0 global flags; 1 known issue measured within its recorded bounds (#31) |
| `audio-health`, `asan-audio-health` | 72 checks, 0 failures |
| `installer-version` | 0.1.0.34 |
| `installer-payload` | 26 extracted files byte-identical to the portable distribution |
| `codec-redistribution`, `codec-distribution-guards` | pass |
| `gui-startup` | demo panel reached its message loop |
| Remaining service, path, boot-mode, endpoint, Windows-audio, spatial, plugin-discovery and AddressSanitizer checks | pass |

Also established on this machine:

- Driver builds with `/WX` and zero warnings in Release and Debug, with `/CETCOMPAT`
  and `/Qspectre`; InfVerif and ApiValidator run in every driver build.
- The panel's tab order, SBC XQ checkbox and Connect / Disconnect buttons were checked
  visually on screenshots.
- LDAC is covered by encoder and parameter tests only; independent LDAC decoding is
  still not part of verification.
- The CI workflow `.github/workflows/verify.yml` exists but is manual-only
  (`workflow_dispatch`) and **has never run**.

## Hardware results (2026-10-13)

Windows 11 build 26300, Qualcomm FastConnect 7800, development certificate and Test
Mode. These are results on one PC; they do not establish compatibility with other
adapters or receivers.

**Sony CMT-SBT40D (SBC only), driver 0.1.0.19:**

- SBC 48 kHz dual channel, bitpool 53 = **672 kbps**, as with 0.1.0.18. A 15-second
  transport check sent 1,928 packets with no transport errors.
- Three software reconnects of about **2.39 s** each, each back at 672 kbps (0.1.0.18:
  2.18-2.60 s). The saved volume stayed stable across reconnects.
- Not re-tested on driver 0.1.0.20 or 0.1.0.21 (the speaker was off). SBC XQ under the
  0.1.0.20 rule has not been played on it.

**EDIFIER MR5, driver 0.1.0.20:** it advertises three SBC endpoints and LDAC, AVDTP 1.4,
media MTU 679.

- LDAC SQ (660 kbps): 15-second check passed.
- LDAC HQ (**990 kbps**): 30-second check passed, 5,777 packets, 0 drops, 0 write
  failures, 0 latency trims; worst write completion about 11.7 ms.
- Three reconnects of about **0.28 s** each.
- Owner listening check: no crackle on LDAC HQ.
- SBC on the MR5 was not tested.
- Windows reports Windows Sonic, Dolby Atmos for Headphones and DTS Headphone:X as
  supported on its endpoint; spatial playback itself was not tested.
- Pair the classic "EDIFIER MR5" entry. "EDIFIER BLE" is a control channel only and
  carries no audio.
- A new device's LDAC ceiling is SQ, unchanged from earlier releases; **Auto quality +
  codecs** or the LDAC tab can choose HQ.

**Codec-host restart during LDAC playback** (the installer restarts the service): the
driver fell back to kernel SBC as designed, and several seconds of audio were dropped
(PCM ring overflow) during the codec switch. LDAC returned at the next stream start.

**Installation.** Driver 0.1.0.21 and application 0.1.0.34 installed without a reboot
while no audio was playing, and the driver reports 0x00010015. Installing while audio
plays leaves the old driver loaded until a reboot, because `pnputil` cannot restart a
device that is pending a reboot. Stop playback before installing.

**Driver binding.** A newly paired A2DP receiver bound to the installed PN-A2DP driver
automatically: its INF outranks Microsoft's by date. **Driver > Use Windows mode** moves
a receiver back.

**Connect / Disconnect** (panel buttons and Windows Settings) on driver 0.1.0.21:
tested by the owner and reported as working. There is no automated log.

## Known limitations

- **Dual-channel SBC ABR floor (#31).** Under heavy congestion dual-channel SBC can fall
  to bitpool 8 (about 132 kbps), which is audibly distorted. The harness bounds it
  (THD+N -48.4 dB at -6 dBFS, pink-noise SNR 8.4 dB, HF burst +15.6 dB) and fails if it
  gets worse.
- **No limiter or headroom (#13).** 0 dBFS programme can clip in the receiver's decoder.
- **AAC at 88.2/96 kHz (#14)**, only with `EXPOSE_ALL_RATES`: same audio bandwidth,
  more air time.
- **aptX** high-frequency overload is codec-inherent; aptX LL packet sizing below an
  895-byte MTU trades up to 2.3 ms of latency for fewer packets.
- **Codec-host restart** during playback drops several seconds of audio while the
  driver switches to kernel SBC; the better codec returns at the next stream start.
- **Install while playing** needs a reboot before the new driver loads.
- **New pairings bind to PN-A2DP** while it is installed.
- `pna2dpctl trace` and `sbc_capture` need an elevated prompt; codec host console mode
  must run as LocalSystem.
- The panel's maximum-buffer slider still goes to 1000 ms; the driver stores at most
  500 ms.

## Remaining verification

Nothing below has been exercised; each needs hardware or an environment not used here.

- Playback measurements on driver 0.1.0.21 (the figures above were taken on 0.1.0.19
  and 0.1.0.20), and the Sony on 0.1.0.20/0.1.0.21, including SBC XQ at 672 kbps.
- A logged Windows Settings Connect / Disconnect cycle.
- Receiver-initiated flows (the 1.5 s OPEN wait, collision handling, codec upgrade),
  and the acceptor answering while our command is outstanding.
- Multipoint behaviour and receiver SUSPEND handling, including the resume detector.
- Sleep/wake, endpoint power-down while the system stays in S0, and a confirmed
  physical power cycle or out-of-range recovery.
- Media stall probe, ACL-loss handling and reconnect back-off over a real link.
- Codec host quarantine and recovery over Bluetooth.
- The remote-feature query (`IOCTL_BTH_GET_RADIO_INFO` for the receiver) that decides
  3-DH5 packing for SBC XQ; if BthPort does not answer it, XQ packs for 2-DH5.
- 2:1 decimation and float formats on a receiver.
- AAC and aptX-family receivers; SBC on the MR5; independent LDAC decoding.
- The control-role token code, event-log entries and ETW output on a running system.
- Driver Verifier; HLK; Secure Boot or memory integrity (HVCI) enabled.
- The CI workflow on GitHub.

Open items: the role capture does not check the impersonation level (see the rejected
round-1 finding); the analysis ratchet compares counts only; quarantine thresholds, the
0.8 throughput budget, the 10 ms slow-write allowance and the per-codec sink delays are
policy choices without hardware validation.

## Next hardware checks

Only with the owner's approval, on the development PC. Have the recovery steps ready:
`.\build\switch-device.ps1 -Rollback`, the crash guard and recovery environment in
[build, sign and install](04-build-sign-install.md#first-load-on-a-development-machine),
and Safe Mode uninstall in the
[Test Mode guide](17-test-mode-release-guide.md#return-to-normal-windows).

1. Sony on driver 0.1.0.21:
   `test-receiver-codec.ps1 -InstanceId '<exact Bluetooth A2DP instance ID>' -ExpectedCodec SBC -Seconds 15`
   (expect 48 kHz, 672 kbps, MTU 895), then with SBC XQ on (expect 672 kbps), then
   `test-connection-recovery.ps1 -InstanceId '<exact Bluetooth A2DP instance ID>' -Cycles 3 -ExpectedSbcKbps 672`.
2. MR5 on driver 0.1.0.21: LDAC HQ for 30 seconds and three reconnects; SBC.
3. Windows Settings **Disconnect** then **Connect** on each receiver, with
   `pna2dpctl watch` running; playback should resume.
4. While streaming, switch a receiver off or move out of range, then restore it. Save
   `pna2dpctl trace` (elevated) and `pna2dpctl stats`.
5. Sleep and wake while connected and while playing.
6. Optional: Driver Verifier for `pna2dp.sys` on one boot (`verifier /standard /driver
   pna2dp.sys`; remove with `verifier /reset`). A Verifier stop during start counts
   toward the crash guard.
7. Record the results in [testing](06-testing-plan.md) and here.
