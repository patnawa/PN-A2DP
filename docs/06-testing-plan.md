# Testing and verification

## Local verification, 2026-10-11

Machine: Windows 11 Pro build 26300, x64, Qualcomm FastConnect 7800 Bluetooth.
MSVC 14.44, SDK/WDK 10.0.26100, KMDF 1.31. The Sony CMT-SBT40D has been installed
with the test-signed PN-A2DP driver after a user-initiated restart. Test signing is
active; Secure Boot and HVCI were off at preparation. Real SBC music playback,
normal loudness, Windows volume/mute, clear bass and the corrected speaker icon
were confirmed by the user. This receiver advertises SBC only.

| Check | Result and coverage |
|---|---|
| Release build/package | Driver, service, CLI, panel, installer and three external plugins build |
| Debug driver/MSVC analysis | Builds; warnings reviewed, including correction of an invalid null output argument in the kernel timing helper |
| Core regressions | Production negotiation, PCM, RTP, AVDTP service parsing/reassembly, receiver classification and transport/LDAC policy; exact count in the latest core.log |
| Malformed inputs | 50,000 deterministic packet inputs through production parsers; also passes AddressSanitizer |
| SBC self test | Eight configurations plus five full-scale bass/transient cases: correlated and opposite-phase stereo, live bitpool, frame sizes and CRC |
| Independent SBC decoding | FFmpeg decodes 13 vectors; full-scale transient regression improved from approximately -4.5 dB SNR to 54 dB after widening joint-stereo arithmetic |
| Plugin matrix | 316 cases with loud bass, full-scale correlated/opposite-phase transients and separate stereo tones, including continued encoding after live reconfiguration; AAC four rates/mono/stereo/CBR/VBR, aptX/HD/LL two rates, LDAC four rates/three modes/four PCM formats/three qualities, two MTUs |
| Independent plugin decoding | 28 AAC, aptX, aptX HD and aptX LL vectors decoded by FFmpeg, checking delay-aligned signal error, gain and channels; LDAC decoding is not covered |
| Shared control helpers | Configuration text, presets, error text, device enumeration and signing query pass |
| AddressSanitizer | Core, controls and SBC tests pass; kernel and third-party DLLs are not instrumented |
| Driver INF | WDK InfVerif /w /v passes |
| Installer payload | All 14 embedded files match the portable distribution byte for byte |
| GUI startup | Demo mode reaches its message loop and closes |
| Uninstall self-deletion | Actual helper compiled into an isolated harness; Unicode, apostrophes and literal percent signs pass; unrelated files are preserved |
| WASAPI bring-up utility | Silent Sony/commercial-driver and Realtek streams drain to their final clock position; seven inactive-endpoint/invalid-argument cases are rejected; endpoint enumeration passes AddressSanitizer |

Run from the repository root:

```powershell
.\build\verify.ps1 -Sign -Sanitize -Interop
.\build\build.ps1 -Target driver -Config Debug -Analyze
```

The verification script saves timestamped logs, results.json, SBC vectors and SHA-256
artifact hashes under out/verification. Interop needs Python and FFmpeg in PATH.
Signing creates/reuses a certificate in the current user's Personal store; it does
not change boot settings, machine certificate trust or device bindings. Omit -Sign
for unsigned builds, or use -SkipBuild to check existing artifacts.

## Corrections from this analysis

- Kernel timing passed NULL to the required output argument of KeQueryInterruptTimePrecise.
  PortCls initialization also discarded the resource list supplied at device startup.
- AAC MPEG-4-only receivers could receive MPEG-2 configurations; the advertised bitrate
  was wrong. Unsupported HE-AAC and unsupported local rates are no longer selected.
- Illegal SBC bitpools and ambiguous or truncated codec configurations were accepted.
- Float positive full-scale could wrap negative, and mono conversion discarded the right
  channel. PCM conversion and the 24-bit ring now preserve sample/frame alignment.
- Host overlapped completions could reorder OPEN/ENCODE/CLOSE. Host errors/timeouts now
  request SBC fallback; results require the registered file owner.
- Oversize AAC access units were dropped. RTP fragmentation now keeps one timestamp
  across fragments and marks the final packet; receiver interoperability remains untested.
- AVDTP fragment labels/types/counts and TLV lengths/duplicates were not strictly checked.
  Queued commands are bounded and tagged with the connection generation; responses must
  match the pending signal ID.
- Parallel Bluetooth readers could reorder signalling. Reader submission/cancellation
  is serialized, and stale receives are ignored.
- Configuration persistence, live volume and SBC ABR ceilings, stream-open allocation
  failures and control-interface startup failures are handled explicitly.
- Event delivery had a lost-wakeup window. Guard-thread reference failure now waits for
  thread exit before driver unload.
- aptX could silently consume only part of an input block; LDAC could silently lose output
  on encoder/queue errors. These conditions now report failure.
- Uninstall now stops when device restoration or package/service removal fails, retaining
  files and registration for retry. Self-deletion avoids a shared temporary batch file
  and shell expansion of special characters in installation paths.

Static analysis is not warning-free. Remaining reports include WDK and callback/SAL
annotations and debug format warnings. The RTP extension warning was reviewed: its
four-bit CSRC count bounds the base header to 72 bytes, and reads are guarded by both
the base length and four-byte remaining-length checks. The optional-MDL free warning
refers to a callee that checks the MDL. These checks do not prove absence of bugs.

## Hardware verification completed and remaining coverage

The Sony negotiated SBC joint stereo at 48 kHz, bitpool 53, approximately 357 kbps,
with an outgoing media MTU of 895. Automatic selection uses the actual advertised
capabilities and installed encoders. It does not force unsupported codecs or bitpools.
The driver and applications are installed in `C:\Program Files\PN-A2DP`.

Version 0.1.0.12 fixed double attenuation by connecting Windows Bluetooth absolute
volume to the topology controls. In 0.1.0.13, the user confirmed clear sound and the
correct Windows icon after fixing loud SBC joint-stereo overflow and classifying the
Sony as a speaker. Live playback retained queued audio during controller stalls,
with zero dropped packets/write failures/latency trims in the observed run. These
observations are bounded tests, not a guarantee against all radio interference.

The normal-user KS regression suite passes 31 checks with audio clients stopped:
control permissions, persistence, volume property bounds, extra event subscription
cleanup, endpoint category, 44.1/48 kHz pin allocation and hardware-latency bounds.
The volume bridge remains active when a diagnostic listener is removed.
The explicit `sbc_capture` diagnostic records just one PCM/SBC packet, requires
write access and cancels an uncompleted capture on timeout. It is not always recording.

`out\Release\audio_test.exe --list` enumerates exact render endpoint IDs and states.
After installing PN-A2DP, use its endpoint ID explicitly:

```powershell
.\out\Release\audio_test.exe --play '<endpoint ID from --list>' --seconds 5
```

This renders a quiet test tone without changing the default output or volume. Add
`--silent` to exercise streaming without sound. The utility checks event delivery,
buffer accounting and device-clock progress, and has bounded waits. Confirm sound
and PN-A2DP packet/error counters separately; a successful WASAPI call does not prove
Bluetooth transmission. As a baseline, silent streams passed through the existing
Sony commercial-driver endpoint at 44.1 kHz and Realtek at 48 kHz. That validates the
utility only; PN-A2DP hardware results are recorded above.

Record the current device instance ID and OEM INF before switching. The helper
build/switch-device.ps1 records the previous INF's SHA-256 for -Rollback. OEM numbers
can be reused: a legacy record without a hash, a missing INF or a hash mismatch is
rejected. Uninstall instead restores Microsoft's inbox driver. The original Sony
package is backed up under `out/verification/previous-driver` on this machine.

1. Trust the test certificate, enable Windows test signing elevated and restart when
   ready. Install the signed package and switch only the intended device. Keep a kernel
   debugger/recovery route available for the first driver load.
2. Confirm no Device Manager problem code, a working control interface and audio endpoint,
   and real SEPs reported by pna2dpctl caps.
3. Play through PN-A2DP; verify audible output and increasing packet/frame counters.
   Exercise pause/resume, volume/mute and supported shared/exclusive formats. The driver
   has no resampler: the selected codec/sink must support the engine's sample rate.
4. Exercise live SBC bitpool/ABR and each codec supported by an actual receiver. An encoder
   ABI test does not establish A2DP interoperability or aptX LL latency.
5. Stop/crash/restart the codec host during playback; verify SBC fallback and recovery.
6. Test receiver-initiated connections, power cycling, radio off/on, sleep/wake, surprise
   removal, rollback/reinstall and SCO/HFP coexistence with a capable receiver.
7. On a recovery-ready test system, run Driver Verifier and sustained playback/device
   churn. Run the relevant HLK Audio, Bluetooth and Device.Streaming suites before release.

Steps 1–3 have been exercised on this Sony. Other receivers/codecs, codec-host failure
recovery over Bluetooth, full power/PnP/SCO stress, Driver Verifier and HLK remain
separate hardware coverage. Independent encoder decoding does not establish receiver
interoperability, acoustic quality in every system, or aptX LL latency.
