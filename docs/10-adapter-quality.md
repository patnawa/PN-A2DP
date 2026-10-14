# Automatic adapter quality

Version 0.1.0.16 identifies the local Bluetooth radio used by the selected receiver,
reads its Classic Bluetooth capabilities, and measures transport behavior while
audio plays. Open **Adapter** in the control panel, or run `pna2dpctl quality`.
Use **Auto quality + codecs** or `pna2dpctl auto` to enable automatic codec selection
and adapter policy. This preserves volume, buffering and the configured bitrate floor.

## What the policy does

- Matches `IOCTL_BTH_GET_LOCAL_INFO` to the profile driver's local Bluetooth address.
  Radio event subscriptions now use that radio, including on PCs with multiple adapters.
  Each radio query has a two-second timeout. Failed queries remain unknown.
- Intersects receiver codec capabilities with available encoders and the Windows
  audio stream's sample rate. A codec's presence in the program does not mean the
  receiver supports it. Since driver 0.1.0.22 the Windows rate is preferred but no longer
  required: when the receiver lacks it, the codec runs at 44.1 or 48 kHz and the driver
  converts the rate (44.1 / 48 / 88.2 / 96 kHz Windows formats), so the codec priority
  decides, not the Windows format. Since driver 0.1.0.23 the Windows rate is the user's
  **Default Format** choice in Sound settings; earlier PN drivers made Windows' shared-mode
  engine use the driver's first listed format (normally 48 kHz) whatever was chosen.
- Starts at the configured quality ceiling on EDR-capable or unknown radios.
  The automatic button selects LDAC HQ, AAC-LC up to 320 kbps, and the receiver's
  SBC limit without forcing an unsupported bitpool. A confirmed Basic Rate-only
  radio gets a conservative 350 kbps starting budget: lower SBC bitpool, LDAC MQ,
  AAC within its ceiling, and fallback past fixed-rate codecs exceeding the budget.
  Receiver minima always take precedence. This budget is a policy, not a bandwidth test.
- Observes write completions, slow completions, failures and audio backlog in
  approximately 250 ms windows. Sustained congestion for one second can reduce
  bitrate, with at least two seconds between reductions. Short isolated spikes
  do not change quality. Completion time is host transport latency, not RSSI.
  Driver 0.1.0.19 changes what counts as slow and how far one reduction can go;
  see [below](#driver-0119-changes-software-verified-only).
- Driver 0.1.0.18 raises quality after 30 seconds of continuously healthy traffic,
  then 10 seconds for the next step and 5 seconds for later steps. Idle periods,
  congestion and long scheduling gaps reset accelerated recovery. A reduction within
  10 seconds of an upgrade requires 60 clean seconds before retrying. Seven upward
  steps take 65 seconds in the uninterrupted healthy-traffic policy test. Older
  drivers require 30 seconds per step. A changing bitrate with clear sound
  and no packet loss can therefore be normal; compare the live transport counters
  and recovery timer before treating the number alone as an audio failure.
- Keeps reductions within the configured floor, negotiated ceiling and codec limits.
  Discrete LDAC and SBC steps may stop above the floor. If the requested floor is
  higher than the ceiling, the ceiling wins. AAC supports the 256/192/128/96 kbps
  ladder within those bounds. SBC uses bitpool steps; LDAC uses HQ/SQ/MQ.

The adapter checkbox enables this policy while keeping manually chosen codec
ceilings. Disabling the checkbox keeps those ceilings and permits manual ABR
control. Disabling ABR in the panel also disables automatic adapter policy.
Existing saved configurations retain their choice until automatic mode is enabled;
new device defaults enable it. Since driver 0.1.0.22 the defaults start every codec at
its best level (LDAC HQ, AAC up to 320 kbps CBR, SBC at the receiver's maximum bitpool),
with ABR as the safety net, and a device's saved settings are never replaced by the
defaults ([maximum audio quality](20-max-audio-quality.md)). `pna2dpctl set autoquality=1` enables policy without
resetting codec settings. To turn off adaptation explicitly, set both
`autoquality=0 abr=0`; `abrfloor=128000` specifies the floor in bits per second.

Driver 0.1.0.17 also reapplies the supported SBC channel preference when the
receiver initiates a connection. Previously, the Sony could choose joint stereo
and stay at 357 kbps despite a saved dual-channel preference; a PN-initiated
connection with the same settings reached 672 kbps. Before playback, the driver
now selects from the receiver's advertised capabilities at the Windows stream's
rate and applies the existing adapter limit. Stream reconfiguration compares rate
and SBC channel mode, leaving automatic in-band bitrate reductions alone. Since driver 0.1.0.22 a stream that has already played is not
reconfigured when Windows changes to a rate the converter bridges. A receiver's own
configuration is still moved to the Windows rate before playback, with the saved channel
mode: the Sony's 44.1 kHz joint-stereo offer under a 48 kHz Windows format becomes 48 kHz
dual channel, bitpool 53, 672 kbps, as with 0.1.0.21. (The first 0.1.0.22 build kept the
receiver's 44.1 kHz, reconfigured it to dual channel at 44.1 kHz, 617 kbps, and converted;
the review fixed this.)

## Driver 0.1.0.19 changes (software-verified only)

These changes pass the policy simulations in `core_test`. On hardware, the Sony's SBC
stream held 672 kbps on driver 0.1.0.19 and the EDIFIER MR5 streamed LDAC HQ at 990 kbps
for 30 seconds without drops on driver 0.1.0.20; neither run reported congestion, so the
reduction and recovery paths below have not been exercised on a receiver. The full list
is in [driver review fixes](18-driver-review-fixes.md).

**Signals.** Each write's own send time is measured: its completion minus the later
of its submission and the previous write's completion, so time spent queued behind
other writes no longer counts. A write is *slow* when that send time exceeds the
media time it carries (taken from its PCM frames, so small VBR AAC units are not
misjudged) plus a fixed 10 ms allowance: 18 ms on the Sony path, where 0.1.0.18
compared completion latency, queueing included, with twice the media time plus
10 ms (26 ms). Congestion is attributed to the link only when the link is the cause:
*backlog* counts only while the TX queue was full, a full queue that delivered less
than 90 % of real time counts as slow, and latency trims while the TX queue had room
(the codec host starving the pump) are not congestion. *Pressure* now means packets
dropped because the TX queue was full. A write slower than four times its media time
(at least 20 ms) is a stall and is left out of the throughput estimate.

**Reductions.** A latency trim caused by the link reduces the bitrate at once,
skipping the 1-second confirmation and the 2-second cooldown, but only once per stall
episode; a clean window re-arms it. When the TX queue was full, a reduction jumps to
the first rung at or below 0.8 times the measured sustained payload throughput
(at least four measured writes needed); otherwise it steps one rung. In the
half-throughput simulation this settles after one trim in 500 ms, where the 0.1.0.18
controller needed 15 trims and 12 seconds. Recovery is unchanged: 30, 10, then
5 seconds per step, 60 seconds after a quick relapse. Because one decision can now
skip several rungs, `AbrDowngrades` counts decisions, not rungs, and getting back to
full quality after a deep cut takes longer.

**Ladders.** The SBC ladder gains one rung whose packets fit a single 2-DH5 baseband
packet, so ABR level numbers below the top shift by one. AAC is capped so every access
unit fits one media packet (about 300 kbps at 44.1 kHz with MTU 895, 320 kbps at
48 kHz); its rungs are the 320/256/192/128/96 kbps steps strictly below that cap, so the
first reduction lowers the bitrate even when the cap is below 256 kbps.

**Packet sizing.** SBC frames per packet are also limited to the local radio's largest
baseband packet (after the 4-byte L2CAP header: 1017 bytes for 3-DH5, 675 for 2-DH5,
548/363 for 3-slot EDR, 335 for DH5): the frame count up to the MTU's with the fewest
air slots per frame. The packing is recomputed at every ABR rung and live bitpool change,
so a lower rung also sends fewer packets and no rung overflows the MTU; with unknown radio
data the old MTU rule applies. aptX packets carry 4-byte (aptX HD 6-byte) codewords in
multiples of 8 (160 at most, aptX LL 112). The media MTU the
driver requests is min(1024, one 3-DH5 = 1017) on 3 Mb/s EDR or unknown radios,
675 on 2 Mb/s-only radios and 1013 (three DH5) on Basic Rate radios. The pump uses the
smaller of the receiver's MTU and that cap, except on 2 Mb/s-only radios, where it keeps
the receiver's MTU and lets the SBC packing choose. On this PC's 3 Mb/s FastConnect 7800
the Sony keeps MTU 895 and 3 dual-channel frames (685 bytes) per packet.

**SBC XQ** (`sbc.xq=1`, SBC page checkbox, default off) asks for dual channel,
16 blocks and 8 subbands at the bitpool plain dual channel gets from the same receiver,
MTU and radio (the receiver's maximum within the MTU: bitpool 53 = 617 kbps at 44.1 kHz,
672 kbps at 48 kHz). It never streams below that: the earlier fixed points (47 for a
3-DH5 link, 38 otherwise) gave the Sony 600 or 492 kbps instead of 672, and five
200-byte frames never fit its 895-byte MTU. What XQ adds is the channel mode (a receiver
whose bitpool limit is a joint-stereo budget, e.g. 35, gets 456 instead of 249 kbps) and
the packing: frames are packed to the EDR packet both the adapter and the receiver take
(3-DH5 only with a receiver known to do 3 Mb/s EDR, from `IOCTL_BTH_GET_RADIO_INFO`;
2-DH5 otherwise), choosing the frame count with the fewest baseband slots per frame. The
ABR's "fits 2-DH5" rung trades bitrate for air time only under congestion. XQ has had no
listening or receiver test; some receivers that advertise bitpool 53 for joint stereo may
not decode dual-channel bitpool 53 reliably.

**Stalls.** When media writes stop completing for 2 seconds, the driver probes the
receiver before tearing anything down; a short radio fade that recovers keeps the
stream and the bitrate policy continues from its current level.

## Driver 0.1.0.20: ladder changes from the audio-quality phase

- **SBC packing follows the bitpool.** Frames per packet are recomputed at every ABR rung
  and live bitpool change. Before, a bitpool lowered live and kept across a pause sized
  the packets for the low bitpool, and the next raise sent packets over the MTU, which
  were all dropped. Lower rungs now also send fewer packets: on the Sony, bitpool 26
  costs 321 instead of 500 baseband slots per second.
- **AAC reductions are CBR.** VBR applies only at the top level; each reduction is CBR
  at the rung's bitrate, so the first step lowers the bitrate (see
  [volume and audio quality](08-volume-and-audio-quality.md)).
- **aptX** packets fill the MTU in multiples of 8 codewords, up to a 160-codeword cap;
  the codec's bitrate is still
  fixed and cannot be reduced in band.
- **LDAC** is refused on a media MTU below 679 bytes and the next codec is tried.
- **Codec-host stalls** count once per stall (plus once per further 500 ms), so a short
  hiccup no longer quarantines the host.

**Known issue #31: the dual-channel SBC floor.** With dual channel (the Sony's SBC
configuration at 672 kbps, and SBC XQ), the 128 kbps ABR floor can be reached
at bitpool 8 per channel, about 132 kbps, which is audibly distorted. It is reached only
under heavy, sustained congestion. A higher dual-channel floor (bitpool 19-20, 250-276
kbps) turned codec noise into latency trims and gaps on a link that sustains only
150-200 kbps; the proper fix, reconfiguring to joint stereo below about 357 kbps, is a
mid-stream renegotiation with an audible gap and has not been implemented. The
audio-quality harness records it (THD+N -48.4 dB at -6 dBFS, pink-noise SNR 8.4 dB, HF
burst +15.6 dB) and fails if it measures worse. Joint stereo at the same rate measures
-62.6 dB THD+N.

## Driver 0.1.0.22: latency profiles scale the congestion rules

The buffer settings are offered as latency profiles: **Music** 150 / 450 ms,
**Balanced** 60 / 250 ms (the default, unchanged) and **Low latency** 30 / 120 ms, or any
custom pair (panel Transport tab, `pna2dpctl set latency.profile=music`). The steady-state
delay is the same for all of them, because the ring is drained at once while the link keeps
up; the profile decides how much backlog a hiccup may leave before the latency guard trims.

A bigger buffer only helps if the ABR does not treat its backlog as congestion. Above a
60 ms target, two rules change (Balanced and Low latency keep the 0.1.0.21 rules exactly):

- a *slow* reason counts only while the TX queue was full and the backlog exceeded
  target - 60 ms (90 ms for Music);
- a window whose backlog shrank by at least 10 ms without a trim is *draining*: the link
  has spare capacity again, so neither *backlog* nor *slow* counts.

Failures, drops, the urgent rule for a trim, the 1-second confirmation, the 2-second
spacing and recovery are unchanged. In the `core_test` link simulation (LDAC, a link with
25 % spare capacity and a 300 ms stall every 8 seconds, 120 seconds), Balanced trims 14
times and lowers the bitrate twice; Music never trims or lowers it, with a peak backlog
of 310 ms. When the link falls to 0.8 times the HQ bitrate, Balanced lowers it after
1.0 s and Music after 1.25 s, without a trim. These are simulations. On hardware (EDIFIER
MR5, LDAC, driver 0.1.0.23) Music was applied while playing without interruption, and the
session had no drops, write failures or latency trims; no interference session has been
run with any profile, and Low latency has not been tried on a receiver. SBC packets stay
sized for at most an 80 ms target, so Music does not make 50 ms packets. Details:
[maximum audio quality](20-max-audio-quality.md).

## Driver 0.1.0.23: converted streams and delay reports

Driver 0.1.0.22 was an intermediate build; the profiles and the converter ship in driver
0.1.0.23 (v0.1.0.36). The congestion rules see a converted stream like a native one: the
ring, trims and backlog are counted in Windows (engine) frames, and the converter adds at
most 2.07 ms of reported latency. A Windows format change during playback is bridged by
the converter, so it no longer interrupts the stream with a renegotiation.

The end-to-end latency estimate now uses the receiver's AVDTP delay report even when
delay reporting was not negotiated (the EDIFIER MR5 reports about 364..396 ms on LDAC
without offering the capability; its estimate went from 209 to about 401 ms). The
estimate does not feed the ABR rules, which watch only the transport.

## Limits and interpretation

Bluetooth version, manufacturer ID, LE features and PHY rate cannot establish
usable A2DP throughput or sound quality. The GUI reports adapter capabilities and
observed health separately. Distance, antennas, interference, Wi-Fi coexistence,
USB scheduling, receiver behavior and host load can change while music plays.
The monitor does not claim to measure RF retries, RSSI, acoustic quality or the
currently negotiated PHY rate. It does not change power policies or antenna settings.

aptX, aptX HD and aptX LL have fixed bitrates at a given sample rate. Their health
is monitored, but the driver cannot reduce their bitrate in band. Persistent trouble
on one of these requires a different codec or better radio conditions; the policy
does not repeatedly disconnect playback to force codec changes. Panel 0.1.0.30 can
offer **Quality > Events > Use adaptive SBC** after persistent fixed-rate codec
trouble. This requires an explicit click, saves SBC as the first preference and
enables ABR using the saved SBC settings. The existing
bounded queue, latency guard and encoder-failure fallback remain active.

Automatic quality does not add codecs to the receiver, create lossless bandwidth,
or guarantee uninterrupted playback under all conditions. On physical receivers here,
only the Sony's SBC path and the EDIFIER MR5's LDAC path (SQ and HQ, 44.1 and 48 kHz) have
been exercised, without congestion. Other codecs have software negotiation, policy, encoder
and decoder regression coverage.

## Implementation and verification

`IOCTL_PNA2DP_GET_QUALITY` is an optional read-only extension to ABI 1. Its snapshot
is published under a lock; completion counters use interlocked operations and the
policy state belongs to the pump thread. Older drivers return an unavailable
message in the panel instead of fabricated capabilities.

Regression tests exercise adapter classification, transient and sustained congestion,
cooldown, recovery, idle periods, long observation gaps, disabled adaptation, and
codec floors/ceilings across rates. Encoder results no longer write stale bitrates
back into the desired AAC configuration; plugin reconfiguration failures are
reported to the existing fallback path.

Microsoft references: [local radio query](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthioctl/ni-bthioctl-ioctl_bth_get_local_info)
and [radio information structure](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthioctl/ns-bthioctl-_bth_local_radio_info).
