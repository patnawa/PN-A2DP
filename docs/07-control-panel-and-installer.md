# Control panel and installer

Two user-facing programs sit on top of the driver's control interface and on SetupAPI:

```
tools/common/           pnctl.*   wrapper around the IOCTL interface, names, presets, config text round trip
                        pndrv.*   device nodes, driver store, per-device driver switching, services, signing, elevation
                        pnboot.*  boot settings, development-driver readiness and operation guard
                        pnspatial.* CLI-only Windows spatial compatibility and renderer discovery
tools/pna2dp-panel/     pna2dp-panel.exe   control panel (Win32, common controls, no framework)
tools/pna2dp-setup/     pna2dp-setup.exe   installer / uninstaller with the whole product embedded
```

Both build with the same `cl` / `rc` / `link` invocation as the rest of the tree
(`build\build.ps1 -Target gui`, `-Target setup`, or `-Target package` for everything plus
the distribution folder). No framework, no MSI toolset, no .NET.

## 1. Control panel (`pna2dp-panel.exe`)

The window lists every paired Bluetooth stereo device (every `BTHENUM\{0000110b-...}`
node, present or not) with the driver that serves it. Selecting a device that runs
PN-A2DP opens its control interface; everything on the tabs then refers to that
headset and is saved per headset by the driver (`Services\pna2dp\Parameters\Devices\<bdaddr>`).

| Tab | What it shows / changes |
|---|---|
| Status | Live connection state, codec, format, encoder owner, bitrate and ceiling, estimated latency, sink delay report, MTU, AVDTP version, flags, last NTSTATUS; transport statistics (packets, bytes, drops, queue depth, write completion time, ABR level, codec host round trip); driver version, codec host service state, encoders available. Refreshes on every driver event (pended `WAIT_EVENT` IOCTL on a worker thread) and once a second. |
| Codecs | Live codec/rate/bitrate, receiver support, live state, and saved or edited settings. Activate selected puts a supported codec first and applies it with SBC fallback. Auto quality + codecs restores automatic selection. The preset picker is a loader, not an indicator of saved settings. SBC cannot be disabled. |
| SBC | Max bitpool (slider, or the headset's maximum), min bitpool, bitpool override (XQ), dual channel preference, rate / channel mode / block / subband / allocation masks, live bitrate estimate at 44.1 and 48 kHz. |
| AAC | Target and peak bitrate or sink maximum, VBR, object type (auto / LC / HE-AAC v1 / v2), afterburner, rate and channel masks. |
| aptX | Rate and channel masks for aptX, aptX HD (plus 16/24-bit input) and aptX LL (plus target buffer level). |
| LDAC | Quality (HQ / SQ / MQ / adaptive), ABR step-down, encoder input format, rate and channel masks, bitrate preview. |
| Adapter | Actual radio name/address, Classic EDR capability, live health, effective bitrate range and observation counters. Auto quality + codecs detects receiver support and enables adapter-aware limits with live adaptation. |
| Transport | Target and maximum buffer (sliders), ABR on/off and floor, reconnect delay, persisted volume, behaviour flags (suspend on SCO, auto start, delay reports, expose all rates, software volume). |
| Headset | AVDTP version, SDP features, every remote SEP decoded (rates, channels, blocks, bitpool, AAC object types and bitrate, services) with the raw IEs. |
| Driver | Driver, provider, version, INF, status, **Use PN-A2DP driver** / **Use Windows mode** / **Rescan**. Labelled Test Mode, Secure Boot and Memory Integrity lights; **Requirements...** explains blocked activation. **Windows Security** opens Core isolation; **Windows / Test Mode...** manages next-start Test Mode with confirmation and manual restart. Staged package with stage/remove, codec host service controls and installed plugins. |
| Audio (Windows mode) | Selected endpoint's volume, mute, format and output status, plus Windows Sound/Bluetooth settings links. |

The Spatial tab was removed in 0.1.0.24. Existing Windows spatial selections are
unaffected. Development-driver activation requires verified package files, actual
Test Mode ON, Secure Boot OFF and Memory Integrity OFF. The helper rechecks the
next-start boot mode too. Close the boot-settings dialog before switching drivers;
a shared operation guard prevents concurrent boot and driver changes.

Bottom bar: **Apply** sends the edited `PNA2DP_CONFIG` with `IOCTL_PNA2DP_SET_CONFIG`
(the driver validates, persists and applies live; bitpool changes go in band), **Revert**
reloads, **Defaults** loads the driver defaults, **Import / Export** round-trip the
configuration as text (`pna2dpctl` key=value lines plus an exact hex blob).

### Driver switching (selective driver)

The driver is installed per device node, not globally. `pndrv.cpp` does what Device
Manager's "Update driver > Have Disk" does:

* `StagePackage` = `SetupCopyOEMInf` (puts `pna2dp.inf` + `.sys` + `.cat` in the driver store).
* `InstallInfOnDevice` = `SetupDiSetDeviceInstallParams(DI_ENUMSINGLEINF)` +
  `SetupDiBuildDriverInfoList` + `DiInstallDevice(DIIRFLAG_FORCE_INF)`; needed because the
  inbox `microsoft_bluetooth_a2dp_src.inf` outranks any third-party INF for the generic
  hardware ID.
* `RestoreInboxDriver` enumerates the compatible drivers for the node and installs the one
  from `microsoft_bluetooth_a2dp_src.inf` (fallback: any Microsoft-provided non-OEM INF).
  This also works for a node that currently runs another third-party A2DP driver.
* `UnstagePackage` = `SetupUninstallOEMInf(SUOI_FORCEDELETE)`.

These calls need administrator rights. The panel runs as the normal user and, for each
such action, starts itself elevated with `--op <name> <args>` through the UAC prompt
(`ShellExecuteEx` with `runas`), waits, and reads the result from the exit code
(`ERROR_SUCCESS_REBOOT_REQUIRED` = done, restart needed). When the panel is already
elevated the operation runs in-process.

### Demo mode

`pna2dp-panel.exe --demo` shows a simulated headset (LDAC streaming, four SEPs, live
counters) so the UI can be exercised without the driver. `--page <name>` opens a tab
directly (`--page Driver`).

## 2. Installer (`pna2dp-setup.exe`)

`build.ps1 -Target setup` scans `out\<Config>` and embeds every deliverable it finds as a
`PAYLOAD` resource together with an index (`name|file|subdir|component|size`), so the
installer is a single file that needs nothing else:

```
driver\pna2dp.inf, pna2dp.sys          required
driver\pna2dp.cat, pna2dp-test.cer     present after build.ps1 -Sign
pna2dp-codechost.exe                   required
pna2dp-codec-ldac.dll, pna2dp-codec-aptx.dll + freeaptx.dll, pna2dp-codec-aac.dll
pna2dp-panel.exe, pna2dpctl.exe        required
LICENSE, THIRD-PARTY-NOTICES.md, README.md
```

Wizard pages: Welcome, License, Components (driver, codec host, each plugin, panel,
CLI, Start menu / desktop shortcuts; plugins pull the host in), Folder, Devices (which
paired headsets get PN-A2DP now; the rest keep the inbox driver), Driver verification,
Progress, Finish. Normal installation requires a verified Microsoft driver signature
and catalog hashes for the embedded INF and SYS. Setup does not import certificates
or enable test signing. Development installs require the explicit
`--allow-test-driver` flag and an already prepared, trusted test machine.

The wizard runs unelevated; pressing Install writes the answers to a temp file and
relaunches itself elevated with `--elevated-install <file>`, which jumps straight to the
progress page. The elevated run:

1. verifies the immutable embedded driver package before creating the installation
   folder, stopping services or replacing files, then writes selected components;
2. driver: `SetupCopyOEMInf` and `DiInstallDevice(DIIRFLAG_FORCE_INF)` for each selected device;
3. service: `CreateService` (auto start, restart on failure) and start;
4. shortcuts, `HKLM\SOFTWARE\PN-A2DP\InstallDir`, Apps & features entry
   (`...\Uninstall\PN-A2DP`), and a copy of itself as `uninstall.exe`.

Errors are logged in the progress list and summarised on the Finish page together with
the restart requirement; the first error is the process exit code.

Uninstall (`uninstall.exe`, which defaults to removal, Apps & features, or the Start menu shortcut)
restores the Microsoft driver on every node that uses `pna2dp`, removes the staged
package(s), deletes the service, shortcuts, registry entries and files, and deletes
itself after exit.

Command line:

```
pna2dp-setup.exe --silent [--dir <folder>] [--no-devices]   normal-mode unattended install
pna2dp-setup.exe --check --report <file>                   read-only embedded package verification
pna2dp-setup.exe --verify-package <folder> --report <file> read-only external package verification
pna2dp-setup.exe --allow-test-driver                      prepared development machines only
pna2dp-setup.exe --uninstall --silent                        unattended removal (log: %TEMP%\pna2dp-uninstall.log)
pna2dp-setup.exe --extract <folder>                          unpack the payload (portable use with build\install.ps1)
```

Silent runs reject invalid/non-Microsoft packages before elevation or installation
changes, writing the failure to `%TEMP%/pna2dp-setup.log`. Accepted runs elevate,
write the log to the install folder and return a Win32 error code (`3010` = success,
restart required). Removing a `.cer` file does not turn a test build into a release.

## 3. Distribution (`build.ps1 -Target package`)

Builds everything, the three plugins (if the submodules are checked out) and the
installer, then collects in `out\<Config>\dist`:

```
pna2dp-setup.exe          the installer
portable\                 loose files + install.ps1 / update-device-driver.ps1 for script-based installs
pna2dp-portable.zip
```

Use `build.ps1 -Target dist -Production -DriverPackage <Microsoft-signed-folder>`
for normal-mode packaging. It verifies the driver and preserves Microsoft's signed
files without rebuilding them. `prepare-submission.ps1` prepares a CAB and manifest;
see [the signing guide](11-production-signing-th.md) for account requirements,
WHCP/HLK versus attestation, and the remaining normal-mode hardware validation.

## 4. Verification on the development PC (2026-10-11)

The PC has one paired A2DP device (a Sony speaker) bound to a third-party A2DP driver and
no PN-A2DP instance; Secure Boot, test signing and memory integrity are off.

| What | How | Result |
|---|---|---|
| Helper library (`tools/common`) | `out\Release\pnctl_test.exe` (built by `-Target tests`): config text round trip incl. hand-edited keys over the blob, presets accepted unchanged by the driver's validation and leaving unrelated settings alone, SBC/LDAC bitrate figures, SetupAPI error text, address parsing, device enumeration | passes |
| Control panel UI | every tab in `--demo` and real mode driven with window messages and captured; dirty tracking, Apply/Revert/Defaults, presets, codec order and enable boxes, SBC/Transport clamping, export/import, About, device switching, close prompt | issues found were fixed (below) |
| Installer UI | every wizard page, back/next/cancel/close, licence gating, component dependencies, folder validation, device list | issues found were fixed |
| Installer payload | `--extract`, SHA-256 of every file against the build outputs; portable zip entry names | identical; `/` separators |
| Install / uninstall (elevated) | silent install to a test folder with `--no-devices` (device driver untouched), reinstall over the running service, panel `--op svc-stop / svc-start / rescan / stage`, codec host console run, silent uninstall through `uninstall.exe` | service, Start menu, Apps & features, registry and files created and fully removed again, uninstaller deletes itself; staging the unsigned package fails with the expected "no catalog" error |

The initial demo-only checks above were followed by live PN-A2DP installation on the
Sony CMT-SBT40D. Normal-user settings persistence, status/capability queries, Windows
volume/mute and music playback now work. Failed Apply retains edits, and the installer
checks locked payloads before replacing files. See `docs/06-testing-plan.md` for the
current hardware evidence and remaining coverage.

The **Automatic** codec button detects receiver capabilities, clears incompatible
manual restrictions and begins at high quality (LDAC HQ or AAC up to 320 kbps), while
retaining SBC fallback and congestion adaptation. Unsupported codec pages are disabled.
The CLI equivalent is `pna2dpctl auto`. Windows receiver volume is selected automatically;
the local gain path is used when receiver-volume integration is unavailable.

### Defects found by this pass and fixed

* Panel: codec enable edits were lost when the headset list refreshed; Apply wrote back a
  stale persisted volume; the status page called any non-PN driver "Microsoft"; the event
  thread could hang the window on close and stale events from a closed session were
  processed; re-entrant refresh during a prompt; reserved fields (AAC peak / afterburner,
  aptX channel and input masks) looked editable but are not used by the driver (now
  greyed); clipped status line and list columns; SBC XQ figures were wrong (bitpool 47
  dual channel = 551 kbps, not bitpool 76); the driver signer lookup looked in the wrong
  folder.
* Installer: a drive root such as `C:\` was accepted as the install folder; an already
  elevated wizard never started the work; reinstall over a running service raced the old
  process; the uninstaller left itself and the log behind; `uninstall.exe` without
  arguments ran the installer; an unsigned package was offered as installable (now
  unchecked with an explanation); dangling string pointers in the device list; failures
  of the uninstaller copy were not reported; the Finish page could not show a long log.
* Library: device "present" was derived from the driver state; SetupAPI errors had no
  text; config import ignored edited keys and accepted any text; presets overwrote
  unrelated settings; the inbox-restore fallback could pick bth.inf's generic service.
* Build: stale catalogs were embedded around a rebuilt unsigned driver; `install.ps1`
  assigned to the read-only `$host` and skipped `freeaptx.dll`; non-ASCII checkout paths
  broke the installer resource script; a missing plugin submodule aborted the package.

### Control panel 0.1.0.26: service shutdown and responsive operations

Stopping, upgrading or removing the codec host waits when shutdown has already
started, instead of issuing a duplicate STOP and failing with error 1061. A race
with another stop request is accepted only after confirming the service is stopped
or stopping. Upgrade never opens a process ID reported in a state where Windows
does not guarantee its validity. Status, process and timeout errors remain visible.

Driver/service operations run in a helper process even when the panel is already
elevated. The waiting panel processes Windows messages while its controls are
disabled and device updates are deferred. This prevents a stalled message loop
and duplicate actions. Cancellation restores the previous input state; wait and
exit-code failures are reported rather than interpreted as successful operations.

### Control panel 0.1.0.25: settings and lifecycle reliability

Settings import reads at most 1 MiB and requires valid UTF-8. Malformed, truncated
or incompatible blobs reject the import without replacing existing settings.
Comments containing `blob=` no longer hide the real saved configuration; negative
and overflowing numeric preferences cannot wrap into unintended values. Export
writes and flushes a temporary file before replacing the destination, and reports
write/replacement errors rather than claiming success.

Device recovery retains its retry limit across session reopenings. File dialogs
and prompts defer driver events, then process them for the current session.
Device identities passed through dialogs and Windows audio COM calls are owned
copies, and stale volume/mute results cannot overwrite a newer selection.

Refresh, device selection and driver operations offer Cancel when settings are
unsaved. A driver operation asks about edits before closing the control session.
Demo mode intercepts all driver/service operations before elevation. Uninstall
stops when device enumeration or service shutdown fails, keeping the installation
available for retry. Shutdown also propagates process/status/wait errors.

### Control panel 0.1.0.20: visible version and program description

The upper-left brand area shows the panel version and the description
"Bluetooth audio, tuned for you". The sidebar, About dialog and executable
metadata share the release definition in `tools/pna2dp-panel/version.h`.

### Control panel 0.1.0.19: readable device status and refreshed layout

The left device list now uses cards with separate device-name, driver and
state/codec lines. It retains native list selection, keyboard navigation and
accessibility data; an infotip exposes the full text for long names. A persistent
header reports the selected output's live state, codec, bitrate and sample rate.

The window can resize and maximize. Layout dimensions follow the window DPI,
with a bounded content width on large monitors. Flat tabs, a light background
and distinct device selection make the information hierarchy clearer. Windows
high-contrast colors are used when that accessibility setting is enabled.

The footer distinguishes saved settings from unapplied edits. **Apply changes**
becomes blue when changes can be saved; **Undo changes** reloads the driver's
current configuration. Codec policy and the audio driver are unchanged.

### Control panel 0.1.0.18: codec parameter pages

SBC, AAC, aptX and LDAC preference masks now have explicit **Auto** checkboxes.
Selecting Auto clears the group's restrictions; choosing an individual option
leaves Auto. Clearing the last option returns to Auto, matching the driver's
zero-mask semantics. Applying automatic settings preserves zero masks.

Each page identifies saved versus edited preferences and reports receiver
support and the actual live codec, bitrate and format. These readouts refresh
without overwriting edits. Unsupported codecs show their saved preferences with
an explicit inactive/unsupported message; they cannot become active on the Sony.

The SBC receiver limit comes from valid audio sink capabilities, preferring the
negotiated endpoint. Missing capabilities remain unknown instead of assuming 53.
The automatic AAC bitrate field no longer invents a 320 kbps saved value. aptX
stereo and HD 24-bit input indicators describe fixed encoder requirements while
preserving the reserved values in the stored configuration.

Validation covers source/non-audio/malformed/truncated capabilities, selected
endpoint limits, live versus disconnected status, and the four pages on the Sony.
This is a panel update; the signed driver remains 0.1.0.16.

### Control panel 0.1.0.17: codec activation and accurate settings

The Codecs page separates enabled preferences from receiver support and the live
codec. Its settings column reads the current configuration (or unsaved edits),
instead of generic bitrate examples. The preset picker starts with an explicit
placeholder; it no longer labels every configuration as Balanced. Merely selecting
a preset previews its description. Loading it creates edits; Apply saves them.

Select a row and press **Activate selected** to save current edits, move that codec
to first priority, enable it and request live negotiation. The receiver must advertise
an audio sink endpoint for the codec and an encoder must be available. Unsupported
or unknown capabilities disable activation. Negotiation still obeys the receiver's
format limits, current Windows sample rate and adapter policy. SBC remains the
fallback, and the live readout shows what was actually negotiated.

The panel's live codec/status refreshes once a second without rebuilding the list
or losing selection. Selection is preserved by codec identity across a list reload.
The host-encoder preference label also reflects the driver's actual SBC fallback.

This is a user-interface update; the signed driver remains at 0.1.0.16. Verification
on the Sony CMT-SBT40D covered SBC activation, refusal to activate unsupported
LDAC, automatic mode restoration, preset preview/load/revert, and agreement with
`pna2dpctl config` and `status`. Tests also cover missing encoders, source-only and
non-audio endpoints, invalid codec IDs, and preservation of volume and buffering.
