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
  receiver supports it.
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
new device defaults enable it. `pna2dpctl set autoquality=1` enables policy without
resetting codec settings. To turn off adaptation explicitly, set both
`autoquality=0 abr=0`; `abrfloor=128000` specifies the floor in bits per second.

Driver 0.1.0.17 also reapplies the supported SBC channel preference when the
receiver initiates a connection. Previously, the Sony could choose joint stereo
and stay at 357 kbps despite a saved dual-channel preference; a PN-initiated
connection with the same settings reached 672 kbps. Before playback, the driver
now selects from the receiver's advertised capabilities at the Windows stream's
rate and applies the existing adapter limit. Stream reconfiguration compares rate
and SBC channel mode, leaving automatic in-band bitrate reductions alone.

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
or guarantee uninterrupted playback under all conditions. Only the Sony SBC path
has been exercised on physical receiver hardware here. Other codecs have software
negotiation, policy, encoder and decoder regression coverage.

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
