# Testing and verification

## Review fixes, application 0.1.0.34 / driver 0.1.0.21 (2026-10-13)

Full record: [driver review fixes](18-driver-review-fixes.md).

**Software.** `build/verify.ps1 -Sign -Sanitize -Interop` passed all **34** checks:
**57,229** core checks (26,199 at 0.1.0.30), also under AddressSanitizer, including
50,000 malformed packet inputs; the new end-to-end audio-quality harness
(`tools/audio-test/quality_test.cpp`, `build.ps1 -Target quality-tests`, run by
`verify.ps1 -Interop`) with **977** jobs and **0** defect flags, plus one known issue
measured within its recorded bounds (#31, the dual-channel SBC ABR floor at bitpool 8,
about 132 kbps, audibly distorted under heavy congestion); FFmpeg independent decoding
of SBC **19/19** and AAC/aptX/aptX HD/aptX LL **28/28**; 72 audio-health checks; the
driver with `/WX` and zero warnings in Release and Debug, `/analyze` at its baseline of
23 warnings (all in SDK/WDK headers), ApiValidator "All binaries are Universal", a valid
INF, `/CETCOMPAT` and `/Qspectre`. LDAC still has no independent decoder test. The CI
workflow is manual-only and has not run.

**Hardware** (Windows 11 build 26300, Qualcomm FastConnect 7800):

| Receiver / action | Driver | Observed result |
|---|---|---|
| Sony CMT-SBT40D, SBC only | 0.1.0.19 | SBC 48 kHz dual channel, bitpool 53 = 672 kbps; 15-second transport check, 1,928 packets, no errors; three software reconnects of about 2.39 s, each back at 672 kbps; saved volume stable across reconnects |
| EDIFIER MR5 (advertises SBC and LDAC; AVDTP 1.4; media MTU 679) | 0.1.0.20 | LDAC SQ 660 kbps, 15 s: pass. LDAC HQ 990 kbps, 30 s: pass, 5,777 packets, 0 drops, write failures or latency trims, worst write completion about 11.7 ms. Three reconnects of about 0.28 s. Owner listening check: no crackle on LDAC HQ |
| Codec-host service restart during LDAC playback (by the installer) | 0.1.0.20 | Fallback to kernel SBC as designed; several seconds of audio dropped (ring overflow) during the switch; LDAC returned at the next stream start |
| Install of driver 0.1.0.21 and app 0.1.0.34 | 0.1.0.21 | No reboot needed with no audio playing; driver reports 0x00010015. Installing while audio plays leaves the old driver loaded until a reboot |
| Newly paired receiver | 0.1.0.20 | Bound to PN-A2DP automatically (its INF outranks Microsoft's by date) |
| Connect / Disconnect (panel and Windows Settings) | 0.1.0.21 | Owner-tested, reported pass; no automated log |

Not covered: playback measurements on driver 0.1.0.21; the Sony on 0.1.0.20 or later
(including SBC XQ); SBC on the MR5; AAC and aptX-family receivers; spatial playback
(Windows reports Sonic, Dolby Atmos for Headphones and DTS Headphone:X as supported on
the MR5 endpoint); sleep/wake; a confirmed physical power-cycle recovery;
receiver-initiated and multipoint flows; Driver Verifier and HLK.

## Reliability update, application 0.1.0.30 / driver 0.1.0.18 (2026-10-12)

Built and installed on this Sony/Qualcomm machine. **29 software groups** passed,
including AddressSanitizer, 26,199 core checks, 50,000 malformed inputs, expanded
codec validation and 54 diagnostic/history checks. **7 Windows-mode** and **7
signing-policy** groups passed. The elevated installer rejected a user-writable
destination with error 5 before creating files or changing the service.

The loaded driver reports 0x00010012. Sony SBC held **672 kbps**, with no new
transport errors in the 15-second observation and three reconnect cycles.
Reconnects took **2.18–2.60 seconds**; **31 live KS checks** passed. Exact settings,
default output, volume and mute were retained. The user reported a physical power
cycle, but monitoring did not observe disconnection; it is not a verified recovery
cycle. The user subsequently confirmed clear music, normal loudness and working
Windows volume/mute on this update. Other receivers/codecs and sleep/wake remain pending. The
[detailed record](16-reliability-improvements.md) separates evidence from remaining work.

## Release audit, application 0.1.0.29 (2026-10-12)

Historical audit snapshot: prepared locally; **not installed at that time**. The
installed app/host were 0.1.0.28 and the reused kernel was test-signed 0.1.0.17. This run made no driver,
service, security-setting or endpoint changes.

- The old full installer failed all three external-codec notice/source checks.
  Rebuilt full installer and portable payloads pass with complete licenses,
  corresponding sources, rebuild instructions and matching hashes.
- `test-codec-distribution-guards.ps1` exercises the production packaging gate:
  complete artifacts are accepted; 14 missing/stale-artifact cases are rejected.
- `test-plugin-discovery.ps1` reproduced loading a benign AAC plugin from a
  simulated ProgramData directory through the host's actual discovery path.
  After removing that search path, ProgramData/current-directory plugins are
  ignored and installed-directory plugins plus built-in SBC remain available.
  `--list-plugins` does not open devices or change service registration.
- All AAC, aptX and LDAC source archives rebuilt successfully in separate
  directories without Git metadata, using `-SkipSourceBundle`.
- `verify.ps1 -SkipBuild -Interop`: **20 groups passed**, including 25,899 core
  checks, 50,000 malformed packet inputs, codec tests, independent decoding,
  payload matching and GUI startup. Windows-mode checks: **7 passed**;
  installer signing-policy checks: **7 passed**. AddressSanitizer was not rerun
  for this packaging/host-discovery update; prior results are recorded below.

Logs: `out/commercial-audit`; verification report:
`out/verification/20261012-181626/results.json`. This is software verification,
not new audible receiver testing. Custom service-path enforcement was subsequently
implemented and tested in 0.1.0.30 above. A wider security review, clean-machine
tests, other receivers, LDAC independent decoding and production signing remain
open. See [release readiness](15-commercial-readiness.md).

## Connection recovery and installer, app 0.1.0.28 / driver 0.1.0.17 (2026-10-12)

Two regressions were reproduced and fixed on the same Sony/Qualcomm machine:

- Full setup and Apps & features reported the kernel package version instead of
  the application release; the installer's numeric version was also fixed at
  0.1.0.0. Setup now shares the panel's release version. Packaging rejects a stale
  panel binary, and both full and Windows-mode metadata are checked automatically.
- Receiver-initiated SBC playback retained joint stereo at 357 kbps even though
  the saved dual-channel preference produced 672 kbps after a local reconnect.
  Driver 0.1.0.17 selects the supported SBC channel mode before playback, including
  when the sample rate already matches. The core regression uses the Sony's real
  capabilities and checks same-rate reconciliation, joint-only receivers and scope
  isolation from AAC. Bitrate-only adaptation does not trigger reconfiguration.

The metadata loop failed six checks before the fix. The codec loop failed one of
25,899 checks before the fix; all now pass. Full verification passed 24 groups,
including AddressSanitizer, independent SBC/AAC/aptX-family decoding, payload
matching and GUI startup. Windows-mode packaging passed seven groups; the rebuilt
driver package passed seven signing-policy checks. These do not establish physical
interoperability for codecs this Sony does not advertise.

| Machine test | Observed result |
|---|---|
| Upgrade with the panel open | Exit 32; payload hashes and running service unchanged |
| Windows-mode and full upgrades | Completed successfully with the panel closed |
| Full uninstall, clean Windows-mode install/uninstall, full reinstall | Completed; uninstall restored the Microsoft driver binding; PN reinstall restored the endpoint and exact saved preferences |
| Initial PnP restart while clients were open | Returned restart-required/pending-restart errors; not counted as a successful restart |
| Idle PnP restart and device disable/enable | Passed after closing clients and stopping the host; host restarted and the endpoint returned |
| Invalid installation root | Rejected with exit 123; installed release unchanged |
| New driver deployment | Setup exit 0; live driver query reports 0x00010011, rather than relying only on the staged INF version |
| Receiver-initiated playback after install | Reconfigured the Sony's 44.1 kHz joint-stereo offer to saved dual-channel SBC at the Windows 48 kHz rate, 672 kbps |
| Device disable/enable on driver 0.1.0.17 | Endpoint ready about 0.55 seconds after the commands completed; subsequent playback reached 672 kbps |
| Three live reconnects on driver 0.1.0.17 | Streaming and receiver absolute-volume integration returned in 2.20–2.52 seconds; 377–378 packets progressed per three-second observation, with no new drops, write failures or latency trims |
| Exact-endpoint silent WASAPI streams | Submitted and drained successfully; packet counters progressed independently |
| Preference and endpoint preservation | Exact configuration unchanged; Windows default selection, 63.5% volume and unmuted state retained |

The user reported power-cycling the Sony, but the monitor did not observe a
disconnect transition. Playback after its return passed; that is not a proven
physical disconnect/reconnect test. Silent streams verify transport, not listening
quality. Sleep/wake, radio power cycling, other receiver codecs and sustained
Verifier/HLK stress remain outside this pass. No boot/security setting was changed
and no system restart was performed.

To repeat the live recovery check, play audio on one explicitly selected receiver:

```powershell
.\build\test-connection-recovery.ps1 -InstanceId '<exact Bluetooth A2DP instance ID>' -Cycles 3
# Optional stable-link SBC quality assertion for this Sony's saved settings:
.\build\test-connection-recovery.ps1 -InstanceId '<exact Bluetooth A2DP instance ID>' -ExpectedSbcKbps 672
.\build\test-installer-version.ps1 -Installed
```

The script interrupts that receiver to reconnect it; it does not change codec
settings, volume, device bindings or boot settings. Reports are local under
`out/recovery-20261012-172746/`, `out/verification/20261012-174625/`,
`windows-mode-20261012-173724/`, `setup-signing-20261012-174726-4d1a4914/` and
`reconnect-20261012-174825-553/`. Older installer and driver backups remain in the
recovery folder. Machine-specific reports are not included in Git.

### Follow-up: reported SBC bitrate drop

During subsequent music playback, the user noticed the bitrate changing without
crackling or interruptions. The trace showed seven automatic reductions, down to
276 kbps, following slow transport writes; the recorded completion maximum was
about 175 ms. No packet drops, write failures or latency trims were recorded.
Read-only observation then verified the complete recovery ladder back to 672 kbps,
one step per 30 seconds of clean traffic. The final observation retained 672 kbps
for at least 50 seconds, with no new queue waits or transport errors.

This confirmed the configured adaptation/recovery behavior rather than a new
codec failure. It does not identify the underlying cause of the earlier host/link
stalls. No driver or user setting was changed during this investigation. Evidence
is retained in `out/bitrate-drop-20261012-175150/`; the core regression suite still
passes all 25,899 checks.

## Audio diagnostics, control panel 0.1.0.27 (2026-10-12)

The new `health_test.exe` exercises the production audio-check model, bounded
quality history and UTF-8 report writer. Its 43 checks cover unavailable and
problem devices, pending restart, endpoint/volume query failure, mute/zero volume,
non-default outputs, receiver support, encoder and codec-host availability,
Windows-mode telemetry limits, stale samples, missing packet progress, counter
resets, observation gaps, congestion/recovery, bounded history and report privacy.
The suite also runs under AddressSanitizer as part of `verify.ps1`.

A resume regression reproduced a false stalled-stream warning twice. Targeted
checks confirmed fresh timestamps and correct zero packet deltas; the history
had retained idle time because the connection state could remain Streaming while
the audio-active flag changed. Resetting the window on that flag transition fixes
the failure. Evidence: `out/health-resume-red*.log`, `health-resume-probes.log` and
`health-resume-green.log`.

Read-only integration on the Sony confirmed endpoint matching, current volume and
mute, default selection, SBC receiver support, encoder registration and service
state. A nonexistent device instance returns an explicit problem and a nonzero
integration-test exit. Live UI checks covered the Sony's rolling transport window,
successful report export, simulated Windows-mode behavior and history/result
clearing on device selection. The GUI report contains capabilities, preferences
and a numeric timeline, without the Sony name/address or endpoint/instance IDs.

Final verification passed 23 software groups, six Windows-mode groups and nine
signing checks. Logs are under `out/verification/20261012-170743/`,
`windows-mode-20261012-170743/` and `setup-signing-20261012-170743-b4ebc8a4/`.
The installed panel, CLI, uninstaller and diagnostics guide match the package
hashes. The installed GUI repeated the audio check and export successfully;
ongoing Sony playback remained SBC 48 kHz / 357 kbps with zero observed dropped
packets, write failures and latency trims. Audio configuration, kernel/host hashes
and the codec-host PID stayed unchanged through the application-only update.

This release changes application diagnostics only. No new kernel/audio-processing
code is installed, and receiver support remains subject to the hardware coverage
documented below. See [audio diagnostics](14-audio-diagnostics.md).

## Diagnosing-bugs pass, control panel 0.1.0.26 (2026-10-12)

Two repeatable failures were isolated using production helpers before applying
fixes. A service already in `STOP_PENDING` rejected a second STOP request with
error 1061, preventing Stop, upgrade and uninstall from waiting for completion.
The upgrade helper also opened a PID whose validity Windows does not guarantee
during shutdown. Tests with longer deadlines still failed immediately; counting
STOP calls and process opens identified the incorrect state handling.

The service regression now covers 20 checks, including an existing pending stop,
a concurrent stop between query and control, timeout without deleting registration,
and an inaccessible stale PID. It substitutes Windows service/process calls and
never mutates a real service. Run the focused loop with:

```powershell
.\build\build.ps1 -Target service-tests -Config Release
.\out\Release\service_failure_test.exe
```

`process_wait_test.exe` exercises the real elevated-helper wait with a worker
thread substituted for process launch. Real Win32 wait handles and synchronous
cross-thread messages reproduce the blocked UI: the helper's message timed out,
even with a longer deadline. The fixed wait dispatches messages, disables owner
input, retains quit messages and propagates wait/exit-code failures. Its 13 checks
also verify owner-state restoration after cancellation and failure. No elevation
or driver operation occurs in this test. Run its loop with:

```powershell
.\build\build.ps1 -Target process-tests -Config Release
.\out\Release\process_wait_test.exe
```

Both loops failed repeatedly before their fixes and pass after them. They are
included in normal verification and AddressSanitizer runs. The panel now launches
driver operations in its helper even when already elevated, with device events
and duplicate commands deferred or suppressed while the helper runs.

The documented API constraints are in Microsoft's
[ControlService](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-controlservice),
[QueryServiceStatusEx](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-queryservicestatusex)
and [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)
references. Red/green evidence is retained in `out/diagnosing-services-*.log` and
`out/diagnosing-process-*.log`. These fixes change application/service management;
the installed signed kernel driver remains 0.1.0.16.

Final verification passed 21 software groups, six Windows-mode groups and nine
installer-signing checks. Reports are under `out/verification/20261012-162941/`,
`windows-mode-20261012-162941/` and `setup-signing-20261012-162941-9128af32/`.
The application-only installer successfully updated this machine to 0.1.0.26;
installed hashes match the package, configuration is unchanged, and the existing
codec-host PID and kernel/host hashes are unchanged. A five-second silent WASAPI
stream submitted and drained 240,000 frames on the Sony at 48 kHz. Bluetooth
packet counters increased with zero drops, write failures or latency trims.
This silent stream checks transport, not subjective listening quality. Other
Bluetooth receiver codecs remain outside this machine's hardware coverage.

## Bug review, control panel 0.1.0.25 (2026-10-12)

Reviewed configuration import/export, device-event recovery, modal dialogs,
Windows audio endpoint ownership, driver operations and installer/service error
handling. Four new configuration-parser regressions failed on the previous code
and passed after the fixes. Configuration-file tests cover Unicode filenames,
UTF-8 BOMs, invalid UTF-8, embedded NULs, empty/missing/oversized files, atomic
replacement and preservation of an existing export when replacement is blocked.
Retry tests cover immediate repeated failures, different devices, manual reset
and system uptime beyond the 32-bit tick rollover.

`service_failure_test.exe` compiles the production service helpers against fake
Windows service/process calls. Its 13 checks inject stop refusal, status-query
failure, timeout, delete failure, inaccessible processes and failed process waits.
No real service is created, stopped or deleted by these tests.

The full verification run passed 18 groups, including AddressSanitizer for core,
SBC and control helpers, encoder tests, independent SBC/AAC/aptX-family decoding,
endpoint discovery, boot policy, payload matching and GUI startup. Windows mode
passed six additional groups; signing policy passed nine. LDAC passes encoder
and packet-layout tests, but has no independent decoder check in this suite.

Interactive demo checks verified the dirty-settings Cancel path, successful file
export and rejection of a malformed blob while retaining edits. Driver operations
in demo mode are now intercepted before elevation, including service controls and
package operations. The signed kernel driver remains 0.1.0.16; this update does
not change encoding or transport algorithms.

Reports: `out/bug-hunt-baseline.log`, `out/bug-hunt-fixed.log`,
`out/bug-hunt-service.log`, `out/verification/20261012-155644/`, and the
`windows-mode-20261012-160007` / `setup-signing-20261012-155722-c4c222ba`
verification folders. Broader hardware limitations below still apply.

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

The initial Sony tests negotiated SBC joint stereo at 48 kHz, bitpool 53,
approximately 357 kbps, with an outgoing media MTU of 895. The 0.1.0.17 recovery
tests above use the saved dual-channel preference at 672 kbps. Automatic selection uses the actual advertised
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
