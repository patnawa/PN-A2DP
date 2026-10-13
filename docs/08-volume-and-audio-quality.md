# Windows volume and audio-quality fixes

## Interoperability findings

Read-only inspection of Alternative A2DP 1.8.3.1 and the local Windows Bluetooth
components identified the interface used by Windows BthAvrcp for receiver volume.
The implementation here is original; no proprietary codec implementation was copied.
Inspection files and extracted installer payloads remain outside the source tree
under ignored `out/verification/alternative-inspection`.

On Windows 11 build 26300, BthAvrcp watches the enabled topology interface
`{e630310c-1733-42bb-9847-7e45d1a27490}` on the AudioSink device instance. Its filter-level
property/event set is `{401e566b-b47e-4e34-b0fc-94826e9e2a1a}`, ID 1. The property is
a LONG in 16.16 dB, from -63.5 to 0 dB. PortCls publishes and owns the interface.
Local changes raise the event; Windows reads the desired volume and sends AVRCP.
Windows SET reports receiver volume back and activates unity encoder gain to avoid
applying attenuation twice. Receiver changes notify the standard audio node events
without echoing another remote-volume event. Digital mute remains immediate.
The last remote listener leaving restores software gain, less the attenuation the
receiver still holds from its last absolute-volume setting: at the moment of the
hand-off that is unity, so a session restart (where Windows re-attaches about 2 s later)
no longer plays the volume twice. Moving the Windows slider after that attenuates in
software relative to the receiver's level. The receiver's held level only counts while the
Bluetooth link stays up: after a link loss (the receiver may power up at another level), or
when the listener leaves only after the link went down, software volume is the user's volume
again until Windows sends absolute volume. Extra listeners are counted.

This is an observed Windows interoperability contract, not a published cross-version
guarantee. If it is not activated, the driver retains local software volume/mute.
`pna2dpctl status` reports which mode is active.

## Loud bass distortion

The SBC analysis filter can overshoot full-scale PCM around transients. Adding or
subtracting its two 32-bit subband values before halving overflowed in joint stereo.
The intermediate now uses 64-bit arithmetic. Full-scale correlated/opposite-phase
square-wave tests reproduce the old defect; FFmpeg independently decodes the fixed
result at approximately 54 dB SNR, versus approximately -4.5 dB before the fix.
The fix is not a bass boost or an equalizer change.

## Congestion and quality

The pump checks available transmission slots before consuming PCM or removing a
queued encoded result. Short Bluetooth stalls retain audio for the next completion;
the existing maximum-latency guard still bounds longer stalls. Diagnostics separately
report transport waits, packet drops and late WaveRT timer callbacks.

LDAC ABR reductions are relative to the configured quality ceiling. A configured SQ
or MQ stream can no longer rise to higher quality in response to congestion. Its
per-codec ABR switch is honored. Automatic selection starts at HQ and backs off when
necessary; AAC targets up to 320 kbps while respecting a lower receiver limit.
Driver 0.1.0.19 also caps AAC so every access unit fits one media packet (about
300 kbps at 44.1 kHz with an MTU of 895); see below.

`build/verify.ps1 -Sign -Sanitize -Interop` runs SBC independent decoding and exports
AAC/aptX plugin vectors for FFmpeg. The plugin matrix covers LDAC encoding at all
supported input formats, quality levels and sample rates. Independent LDAC decoding
remains unverified. LDAC SQ and HQ have played on one physical receiver (EDIFIER MR5,
driver 0.1.0.20; see [testing](06-testing-plan.md)); AAC and aptX-family receivers
remain untested.

## Driver 0.1.0.19 and 0.1.0.20: ramps, crossfades, dither and 24-bit SBC input

Driver 0.1.0.19 introduced these changes; the audio-quality phase in driver 0.1.0.20
refined the start, gap, pause and headroom handling. They are covered by unit tests,
independent decoding and the end-to-end audio-quality harness. On hardware, the owner
heard no crackle on LDAC HQ through the EDIFIER MR5 (driver 0.1.0.20), and the Sony's
SBC transport checks passed on 0.1.0.19; nothing here was measured acoustically on a
receiver. See [driver review fixes](18-driver-review-fixes.md).

- **Volume and mute ramps.** A software volume or mute change ramps linearly across one
  encoder block (one packet's audio, about 8 ms for the Sony's SBC packets) from the
  previous gain to the new one, instead of stepping. The first block of a stream ramps
  in from silence when audio before it was discarded (a restart after an outage); a
  stream that starts at the programme's own start is not touched. Constant gain is
  bit-exact with earlier drivers. Receiver (AVRCP) volume is unchanged.
- **Start of playback.** After 3 s of silence the stream is suspended, and the next
  sound needs an AVDTP START (130-450 ms on the Sony with a sniff exit). The WaveRT clock
  now holds until the stream runs (at most 500 ms), so the beginning of the sound is
  delayed like a device's start-up latency instead of being rendered and discarded.
- **Gaps and pauses.** A packet the audio engine did not write in time is still played
  as 10 ms of silence, but the audio before it fades out and the audio after it fades in
  (2 ms raised-cosine fades; the stream holds the newest 2 ms back for that). At a pause
  or stop, the audio waiting for a whole encoder block fades out and is padded with
  silence that also drains the encoder's delay (AAC: about 33 ms), so the end of a sound
  reaches the receiver; a resume of the same stream fades in. The held-back 2 ms join the
  pump's buffer in the same locked step as that fade, so the pump thread cannot send them
  unfaded first; the engine's end of stream ends the sound the same way. A stream start
  fades in only when the audio it discards ended in a signal (an outage), not after the
  engine's silence while the receiver kept the stream suspended.
- **Headroom.** Float input over full scale, a crossfade and the 2:1 decimator's
  overshoot are carried in 64 bits until the volume has been applied and saturated once:
  at reduced volume, overs no longer clip, also inside a fade. A NaN or infinite float
  sample repeats the channel's last finite sample; a run of them fades to silence over 32
  samples.
- **Latency-trim crossfade.** When the latency guard discards buffered audio, the first
  4 ms of the discarded audio fades into the kept audio instead of cutting. This applies
  to kernel SBC and to host codecs.
- **24-bit SBC input.** The kernel SBC encoder now takes 32-bit input and keeps 24 bits
  into its filterbank, so software volume below 100 % no longer rounds to 16 bits first.
  16-bit sources at unity gain produce byte-identical frames. In independent decoding,
  vectors encoded from 24-bit input give 47.41 / 66.10 / 66.83 dB SNR against
  47.40 / 65.84 / 66.71 dB from 16-bit input; a -70 dBFS tone improves from 20.10 to
  22.39 dB.
- **Dither.** Host codecs that take 16-bit PCM (AAC) receive deterministic TPDF dither
  of ±1 LSB whenever a sample is not already an exact 16-bit value: gain below unity, a
  wider or float source, decimation, or a crossfade. It is never added when muted or to
  a block of digital silence. It adds about 4.8 dB of noise over plain rounding at the
  16-bit level; the tests check that a 0.3 LSB offset and a 0.4 LSB tone survive
  instead of rounding away.
- **2:1 decimation.** A 96 or 88.2 kHz Windows format can feed a receiver at 48 or
  44.1 kHz through a fixed-point half-band filter (passband ripple 0.0009 dB to
  0.45 fs_out, stopband at least -80 dB from 0.55 fs_out). No other rate conversion
  exists.
- **Silence for unwritten packets.** If the Windows engine has not written a packet by the
  time it plays, the driver sends silence rather than stale buffer contents and counts it
  as a starved packet ([audio diagnostics](14-audio-diagnostics.md)).

## Driver 0.1.0.20: codec-side changes from the audio-quality phase

Measured with the end-to-end harness (`build.ps1 -Target quality-tests`, run by
`verify.ps1 -Interop`): 977 jobs, no defect flags in the v0.1.0.34 run.

- **AAC bandwidth.** In CBR the AAC plugin sets the encoder's audio bandwidth to 20 kHz
  at 128 kbps per channel or more and 19 kHz at 96 kbps per channel or more; below that
  FDK's own table applies. Before, FDK's table cut CBR at about 17 kHz. VBR keeps the
  encoder's choice.
- **AAC VBR and ABR.** VBR applies only at the top ABR level. Every reduction is CBR at
  the rung's bitrate (VBR ignored the requested bitrate, so the first reduction changed
  nothing), and the plugin returns to VBR at the top level. The VBR mode follows the
  bitrate per channel: mode 5 from 128 kbps, 4 from 96, 3 from 64, 2 from 48, otherwise
  1. In VBR the peak bitrate is
  the smaller of the packet-fit limit and the bitrate the receiver declared, so VBR
  averages no longer exceed the receiver's maximum.
- **aptX packing.** libfreeaptx emits 4-byte codewords (aptX HD 6-byte); the packet size
  assumed twice that and filled about half the MTU. Packets now carry about twice the
  audio: they fill the MTU in multiples of 8 codewords (the parity sync period) up to a
  cap of 160 codewords (13.3 ms at 48 kHz), so at MTU 895 aptX gets 160 codewords (the
  cap, 640 bytes) and aptX HD 144, instead of about 110 and 73. aptX LL keeps at most 112
  codewords (9.3 ms at 48 kHz): below an 895-byte MTU that is up to 2.3 ms more
  packetisation than before, in fewer packets, a deliberate trade-off.
- **LDAC on a small MTU.** LDAC needs a media MTU of at least 679 bytes. Below that it is
  refused right after OPEN and the next codec is configured, instead of failing every
  encode until the codec host is quarantined.
- **Codec-host stalls.** One stall of the host process counts as one failure, plus one per
  further 500 ms, so a 220-290 ms hiccup no longer quarantines the host and drops the
  stream to SBC.

Known limits, deliberately unchanged:

- **No limiter or headroom.** Programme at 0 dBFS can clip in the receiver's decoder. The
  verifiers measured that error 24-43 dB below the codec's own noise on realistic
  masters; a fixed attenuation would cost loudness on every track.
- **AAC at 88.2/96 kHz** (only with **Expose all sample rates** and a 96 kHz AAC
  receiver) uses more air time at the same audio bandwidth.
- **aptX high-frequency overload** (for example 19 + 20 kHz at -1 dBFS) is inherent to
  the codec; FFmpeg's own aptX encoder gives the same result.
- The dual-channel SBC ABR floor is described in [automatic adapter quality](10-adapter-quality.md).
