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
  If the service is absent or slow, the driver falls back to its in-kernel SBC encoder
  and renegotiates the stream.
* **pna2dpctl.exe** is the control tool (and the reference for a GUI): status, remote
  capabilities, configuration, statistics, event watch, reconnect.

## 2. Data path and timing

1. The audio engine writes PCM into the WaveRT cyclic buffer and advances packets
   (event-driven mode). `CPnMiniportWaveRTStream::Tick` runs every 1 ms on a
   high-resolution timer, computes how many bytes are due at the nominal rate, copies
   them into the pump's PCM ring and signals notification events at packet boundaries.
   The engine is therefore the clock; the Bluetooth path never drifts against it.
2. The pump thread (priority 18) wakes on new PCM or on write completions, converts the
   ring data to the encoder's input format (S16/S24/S32/F32, software volume if
   enabled), and either encodes SBC in the kernel or ships blocks to the codec host.
3. Encoded frames are wrapped per codec (RTP header + 1-byte frame header for SBC and
   LDAC, RTP only for AAC and aptX HD, bare for aptX and aptX LL) and written to the
   L2CAP media channel with a bounded number of writes in flight.
4. A latency guard trims the ring when it exceeds `MaxLatencyMs`; the ABR controller
   watches write completion latency and queue depth, lowers the bitrate ladder
   (SBC bitpool, LDAC EQMID, AAC bitrate) after sustained congestion and raises it again
   after a quiet period. SBC bitpool and LDAC EQMID changes are in band and need no
   AVDTP signalling.
5. Latency is reported back to Windows through `GetHWLatency` and the presentation
   position (ring fill + packets in flight + measured completion time + the sink's AVDTP
   delay report), so players can lip-sync.

## 3. Connection state machine (device.cpp)

Everything is driven by `Reconcile()`, which runs under the device mutex from work items
and makes reality match `AudioRequest`:

```
want = audio RUN && stream open && !(SCO active && SuspendOnSco)
want:
   signalling up?   -> AvdtpConnectSignalling (+ best-effort SDP for the AVDTP version)
   capabilities?    -> DISCOVER + GET_(ALL_)CAPABILITIES, cache, publish to user mode
   rate mismatch?   -> SUSPEND / RECONFIGURE to the engine's rate when the sink allows it
   idle?            -> choose codec (priority list x enabled x encoder available x sink SEP)
                       SET_CONFIGURATION (+ delay reporting), OPEN, connect media channel
   open?            -> start pump, START
not want:
   streaming?       -> stop pump after the suspend timer (3 s) or immediately on SCO/close
                       SUSPEND
```

Remote-initiated flows are handled by the AVDTP acceptor: incoming L2CAP connections to
PSM 0x19 from the paired address become the signalling channel and later the media
channel; remote DISCOVER/GET_CAPABILITIES see our local SEPs (one per codec), remote
SET_CONFIGURATION/OPEN/START/SUSPEND/CLOSE/ABORT/DELAYREPORT update the state and
`Reconcile` reacts.

HCI events from the radio (`GUID_BLUETOOTH_HCI_EVENT`) detect SCO links to the same
address: by default the stream is suspended while a call is active and resumed after.

## 4. Codec selection

`PNA2DP_CONFIG.CodecPriority` (default LDAC > aptX HD > AAC > aptX > aptX LL > SBC) is
walked; a codec is used when it is enabled, the sink has a free sink SEP for it, and an
encoder exists (kernel SBC, or a registered host plugin with pended work requests).
`A2dpSelectConfig` intersects the sink capabilities with user preferences and the
engine's current sample rate and bit depth, preferring the engine rate so that no
resampling happens, and 24-bit input for LDAC and aptX HD.

## 5. User-mode interface

The control interface (`GUID_DEVINTERFACE_PNA2DP`, reference string `pna2dpctl`) lives
on the Bluetooth device node, so one instance exists per headset. IOCTLs are buffered,
versioned (`PNA2DP_ABI_VERSION`) and documented in `driver/pna2dp/inc/pna2dp_public.h`:
version, status, capabilities, get/set config (applied live; SBC bitpool changes in
band), wait-event (pended IRP completed on state/caps/config/SCO/delay events), stats,
reconnect, and the codec-host protocol.

## 6. Differences from the Alternative A2DP Driver

| Area | Alternative A2DP | PN-A2DP |
|---|---|---|
| Codec execution | all in kernel | SBC in kernel, everything else in a user-mode plugin host (crash isolated, LGPL/Apache isolated, upgradable) |
| Fallback | none (no sound without license) | automatic SBC fallback when a host codec is unavailable |
| Applying changes | reconnect required | live: in-band for bitpool/EQMID, AVDTP RECONFIGURE for rate, renegotiation otherwise |
| Latency to apps | not reported | measured path latency + AVDTP delay reports exposed through WaveRT |
| Remote initiated streams | supported | supported, including remote SET_CONFIGURATION with codec validation |
| Status to apps | registry polling | event IOCTL (pended), plus stats |
| ABR | on/off | leaky-bucket controller with hysteresis and codec ladders |
| Licensing | paid, motherboard locked | MIT |
| Signing | attestation signed | test-signed builds; attestation signing requires an EV certificate (see 04) |

## 7. Not done yet (tracked as phase 2)

* Sink role (PC as a Bluetooth speaker) through the `{0000110a-...}` PDO and a capture
  pin; the SBC decoder already exists in `common/sbc`.
* Kernel resampler for sinks that cannot run at the engine's rate (today the format
  table is restricted to supported rates and a mismatch is logged).
* FastStream / aptX LL bidirectional back-channel.
* HLK test pass and attestation-signed releases.
* GUI (WinUI 3) on top of `pna2dpctl`'s IOCTL usage.
