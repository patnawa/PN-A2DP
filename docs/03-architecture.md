# PN-A2DP architecture

PN-A2DP is an open (MIT) Bluetooth A2DP source driver for Windows 10 and 11 that replaces
the inbox `BthA2dp.sys` for a paired headset and gives the user full control over the
codec and its parameters. It is designed from the analysis in `01-alternative-a2dp-analysis.md`
and `02-windows-bluetooth-audio-internals.md`.

## 1. Components

```
+---------------------------------------------------------------------------------------+
| user mode                                                                             |
|  pna2dpctl.exe            pna2dp-codechost.exe (service)         Windows audio engine |
|  status/config/watch      plugin host: LDAC, aptX/HD/LL, AAC,    (shared/exclusive    |
|  (IOCTLs)                 Opus ... via pna2dp-codec-*.dll         WASAPI clients)     |
|       |                          |  GET_WORK / PUT_RESULT              | WaveRT       |
+-------|--------------------------|-------------------------------------|--------------+
| kernel|                          |                                     v              |
|  +----v--------------------------v-------------------------------------------------+  |
|  | pna2dp.sys (KMDF miniport + PortCls adapter)                                    |  |
|  |  control.cpp   codechost.cpp   audio_wave.cpp / audio_topo.cpp (endpoint)       |  |
|  |  device.cpp    state machine / reconcile / codec selection / events             |  |
|  |  pump.cpp      PCM ring -> SBC (kernel) | host results -> RTP -> L2CAP          |  |
|  |  avdtp.cpp     AVDTP 1.3 INT + ACP, delay reporting, reconfigure                |  |
|  |  bthlink.cpp   BRB L2CAP channels, SDP, HCI SCO/ACL events                      |  |
|  +-------------------------------------|-------------------------------------------+  |
|                                        | IOCTL_INTERNAL_BTH_SUBMIT_BRB               |
|  BthEnum.sys (PDO per remote service)  v                                              |
|  BthPort.sys (HCI / L2CAP / SDP)  ->  BthUsb.sys  ->  Bluetooth radio                 |
+---------------------------------------------------------------------------------------+
```

* **pna2dp.sys** binds to `BTHENUM\{0000110b-...}` exactly like the inbox and the
  Alternative driver, so nothing else in the Bluetooth stack changes and uninstalling
  restores the inbox driver. It is a PortCls adapter (one WaveRT render filter, one
  topology filter) and a Bluetooth profile driver at the same time.
* **pna2dp-codechost.exe** is a Windows service that keeps several `GET_WORK` requests
  pended on each device's control interface. The driver completes a request with a block
  of PCM; the host encodes it with a plugin and returns the payload with `PUT_RESULT`.
  If the service is absent, the driver falls back to its in-kernel SBC encoder and
  renegotiates the stream. Since driver 0.1.0.19 repeated host failures (four in a row)
  quarantine the host for 30 s, then 2 min, then 10 min; it counts as absent meanwhile,
  and a stream that fell back to SBC returns to the better codec at the next START or
  pause once the quarantine ends. Only the LocalSystem service may register as a host.
* **pna2dpctl.exe** is the control tool (and the reference for a GUI): status, remote
  capabilities, configuration, statistics, event watch, reconnect.

## 2. Data path and timing

1. The audio engine writes PCM into the WaveRT cyclic buffer and advances packets
   (event-driven mode). `CPnMiniportWaveRTStream::Tick` runs on a high-resolution timer
   (every 2 ms for notification intervals of 5 ms or more, otherwise every 1 ms),
   computes how many frames are due at the nominal rate, copies them into the pump's
   PCM ring and signals notification events at packet boundaries. The engine is the
   clock. Since driver 0.1.0.19 the frames due are computed with an exact remainder,
   so a one-hour simulation shows no drift; the earlier rounding lost up to about
   178 ms per hour. A packet the engine has not written in time is sent as silence.
2. The pump thread (priority 18) wakes when a PCM write completes an encoder block, on
   write completions and on codec-host work, with a 4 ms safety tick. It converts the
   ring data to the encoder's input format (S16/S24/S32/F32; software volume with a
   ramp across one block; TPDF dither for 16-bit host input) and, when the engine rate
   is not the codec rate, to the codec rate (since driver 0.1.0.22: a fixed-point
   polyphase stage and / or a 2:1 FIR, 44.1 / 48 / 88.2 / 96 kHz to 44.1 / 48 kHz, Q28
   coefficients, 64-bit accumulators, alias-free: at least 122 dB stopband from the lower of
   the input and output Nyquist frequencies; see `20-max-audio-quality.md`), and either
   encodes SBC in the kernel from
   24-bit-precision input or ships blocks to the codec host. Each block takes exactly the
   engine frames that make one encoder block (`PnPcmInputFramesFor`); the ring, trims and
   `PcmFramesConsumed` stay in engine frames, RTP time in codec frames, and the
   converter's delay (at most 199 engine frames, 2.07 ms) is part of the reported latency.
3. Encoded frames are wrapped per codec (RTP header + 1-byte frame header for SBC and
   LDAC, RTP only for AAC and aptX HD, bare for aptX and aptX LL) and written to the
   L2CAP media channel with a bounded number of writes in flight.
4. A latency guard trims the ring when it exceeds `MaxLatencyMs` (at most 500 ms;
   the trim crossfades over 4 ms; a further trim before that crossfade has started keeps
   it, since 0.1.0.22). The ABR controller watches each write's own send
   time, TX-queue-full events, failures and backlog, lowers the bitrate ladder
   (SBC bitpool, LDAC EQMID, AAC bitrate) after congestion caused by the link and raises
   it again after 30 seconds of continuously healthy traffic. Idle time does not count.
   SBC bitpool and LDAC EQMID changes are in band and need no AVDTP signalling. The
   target / maximum pair is a latency profile (driver 0.1.0.22, `config_policy.h`): Music
   150 / 450 ms, Balanced 60 / 250 ms (default), Low latency 30 / 120 ms. Above a 60 ms
   target the ABR lets the ring absorb a hiccup: slow writes count only once the backlog
   passes target - 60 ms, and a backlog the link is draining is not congestion. The ring
   is drained at once while the link keeps up, so the steady-state delay is the same for
   every profile. See `10-adapter-quality.md` and `20-max-audio-quality.md`.
5. Latency reaches Windows through the presentation position: PCM in the ring, at the
   codec host, in queued results and in L2CAP writes, plus one write's measured send
   time, plus the sink delay (the AVDTP delay report, or a per-codec default when the
   receiver does not report; since driver 0.1.0.23 a report is used even when delay
   reporting was not negotiated, as the EDIFIER MR5 sends one on its LDAC stream without
   offering the capability). The position changes smoothly (at most 1 % of real time),
   never goes backwards within a run and resets at a stream start or stop. `GetHWLatency`
   reports zero: this software endpoint has no DMA FIFO, and a non-zero value made
   Windows' format probes fail.

## 3. Connection state machine (device.cpp)

Everything is driven by `Reconcile()`, which runs in the device workers under the state
mutex and makes reality match `AudioRequest`. Since driver 0.1.0.19 audio-engine,
KS property and IOCTL threads never take that mutex or wait for Bluetooth I/O: they
record an intent (stream opened or closed, new configuration, reconnect, disconnect)
under a short intent mutex and queue the worker. The AVDTP session has a command lock
(one outgoing procedure at a time, held across its transaction) and a short stream lock
that is never held across a wait, so the acceptor answers the receiver while our command
is outstanding. The lock order is documented in `device.h` and `avdtp.h`.

```
want = audio RUN && stream open && !(SCO active && SuspendOnSco)
want:
   signalling up?   -> AvdtpConnectSignalling (+ best-effort SDP for the AVDTP version)
   capabilities?    -> DISCOVER + GET_(ALL_)CAPABILITIES, cache, publish to user mode
   rate mismatch?   -> SUSPEND / RECONFIGURE to the engine's rate when the sink allows it
                       (not for a stream that has played and the converter bridges)
   idle?            -> choose codec (priority list x enabled x encoder available x sink SEP)
                       SET_CONFIGURATION (+ delay reporting), OPEN, connect media channel
   open?            -> start pump, START
not want:
   streaming?       -> stop pump after the suspend timer (3 s) or immediately on SCO/close
                       SUSPEND
```

Since driver 0.1.0.19: every failed pass arms a retry (1.5 s, doubling, capped at 30 s);
a receiver SUSPEND is respected (no START for 8 s unless playback restarts or the receiver
STARTs, one timed START per episode); a media stall of 2 s is probed with
GET_CAPABILITIES before the stream is aborted; and an ACL-down event closes the session.
Details are in `18-driver-review-fixes.md`.

Remote-initiated flows are handled by the AVDTP acceptor: incoming L2CAP connections to
PSM 0x19 from the paired address become the signalling channel and later the media
channel; remote DISCOVER/GET_CAPABILITIES see our local SEPs (one per codec; a codec that
is disabled or has no registered encoder is reported in use, SBC is always available),
remote SET_CONFIGURATION/OPEN/START/SUSPEND/CLOSE/ABORT/DELAYREPORT update the state and
`Reconcile` reacts. After a remote SET_CONFIGURATION the driver waits up to 1.5 s for the
receiver's own OPEN before sending one. Commands that cross ours are resolved by the
collision rules in `transport_policy.h`.

HCI events from the radio (`GUID_BLUETOOTH_HCI_EVENT`) detect SCO links to the same
address: by default the stream is suspended while a call is active and resumed after.

## 4. Codec selection

`PNA2DP_CONFIG.CodecPriority` (default LDAC > aptX HD > AAC > aptX > aptX LL > SBC) is
walked; a codec is used when it is enabled, the sink has a sink SEP for it, and an
encoder exists (kernel SBC, or a registered, non-quarantined host plugin with pended
work requests). SEPs the sink reports in use are tried last, and a (codec, SEID) pair
the sink refused is skipped for the rest of the session. `A2dpSelectConfig` intersects
the sink capabilities with user preferences and the engine's current sample rate and
bit depth, preferring the engine rate so that no resampling is needed, and 24-bit input
for LDAC and aptX HD. Since driver 0.1.0.22 the engine rate no longer decides the codec:
when the sink lacks it, the stream takes 44.1 or 48 kHz (the engine rate's own family
first, exactly half of 96 / 88.2 kHz first of all) and the pump converts
(`PnStreamRateFor`, `PN_RATECONV_ANY`); LDAC's automatic input is then 32-bit. A Windows format change the converter can bridge keeps a stream that has already played
(no AVDTP RECONFIGURE, `PnStreamBridged`), except a hi-res engine rate above 48 kHz the
sink takes natively. A stream that has carried no audio since it was configured (the
receiver's own SET_CONFIGURATION, or ours made with no audio open) goes to the engine rate
before its first START, since a RECONFIGURE then interrupts nothing; a RECONFIGURE needed
anyway (a channel-mode preference) also targets the engine rate (0.1.0.22 review). 32
and 16 kHz engine rates, 176.4 / 192 kHz and conversion up to 88.2 / 96 kHz are not
bridged.

**The engine format is the user's choice.** Format negotiation with Windows no longer
steers the engine rate. Until driver 0.1.0.23 the wave filter answered
`KSPROPERTY_PIN_PROPOSEDATAFORMAT2` with the first entry of its format table (48 kHz);
Windows uses a driver's answer to that property as the engine format and ignores the
user's Default Format, so every PN endpoint's shared-mode engine ran at that first entry
(48 kHz unless the active stream rate was 96 / 88.2 / 32 / 16 kHz). The property is no longer
supported (`KSPROPERTY_PIN_PROPOSEDATAFORMAT`, the format check, remains), so Windows
opens the format selected in **Sound > device > Properties > Advanced > Default Format**.
The codec choice above then follows that rate: natively when the sink has it, through the
converter otherwise. A receiver's own SET_CONFIGURATION at another rate is still
reconfigured to the engine rate before the first START (for example the Sony's 44.1 kHz
offer under a 48 kHz engine ends at 48 kHz dual channel, bitpool 53); a format change after
the stream has played is bridged by the converter instead of renegotiating Bluetooth. An
application sees a new format only when it reopens its stream.

## 5. User-mode interface

The control interface (`GUID_DEVINTERFACE_PNA2DP`, reference string `pna2dpctl`) lives
on the Bluetooth device node, so one instance exists per headset. IOCTLs are buffered,
versioned (`PNA2DP_ABI_VERSION`) and documented in `driver/pna2dp/inc/pna2dp_public.h`:
version, status, capabilities, get/set config (applied live; SBC bitpool changes in
band), wait-event (pended IRP completed on state/caps/config/SCO/delay events), stats,
reconnect, and the codec-host protocol. Each handle gets a role from the caller's token
when it is opened (`control_access.h`): any user may use the status, configuration,
event, statistics and reconnect requests that the unelevated panel needs; the trace and
SBC sample need an elevated administrator; the codec-host requests and unknown codes
need LocalSystem.

## 6. Differences from the Alternative A2DP Driver

| Area | Alternative A2DP | PN-A2DP |
|---|---|---|
| Codec execution | all in kernel | SBC in kernel, everything else in a user-mode plugin host (crash isolated, LGPL/Apache isolated, upgradable) |
| Fallback | none (no sound without license) | automatic SBC fallback when a host codec is unavailable; a failing host is quarantined and the codec returns automatically |
| Applying changes | reconnect required | live: in-band for bitpool/EQMID, AVDTP RECONFIGURE for rate, renegotiation otherwise |
| Latency to apps | not reported | measured path latency + AVDTP delay reports exposed through WaveRT |
| Remote initiated streams | supported | supported, including remote SET_CONFIGURATION with codec validation |
| Status to apps | registry polling | event IOCTL (pended), plus stats |
| ABR | on/off | time-window controller with hysteresis and bounded codec ladders |
| Licensing | paid, motherboard locked | MIT |
| Signing | attestation signed | test-signed builds; attestation signing requires an EV certificate (see 04) |

## 7. Not done yet (tracked as phase 2)

* Sink role (PC as a Bluetooth speaker) through the `{0000110a-...}` PDO and a capture
  pin; the SBC decoder already exists in `common/sbc`.
* Sample-rate conversion beyond the 44.1 / 48 kHz outputs (to 88.2 / 96 kHz, from 32 or
  16 kHz, from 176.4 / 192 kHz). The converter (driver 0.1.0.22 onwards) covers every Windows
  rate the render pin offers by default (48 and 44.1 kHz) and the hi-res rates up to
  96 kHz; the format table is unchanged.
* FastStream / aptX LL bidirectional back-channel.
* HLK test pass and attestation-signed releases.

The control panel and the installer (per-device driver switching, codec configuration,
packaging) are described in `07-control-panel-and-installer.md`.
