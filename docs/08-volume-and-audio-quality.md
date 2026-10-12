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
The last remote listener leaving restores software gain. Extra listeners are counted.

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

`build/verify.ps1 -Sign -Sanitize -Interop` runs SBC independent decoding and exports
AAC/aptX plugin vectors for FFmpeg. The plugin matrix covers LDAC encoding at all
supported input formats, quality levels and sample rates. Independent LDAC decoding
and Bluetooth playback with non-SBC receivers remain unverified on this machine.
