# Control panel and installer

Two user-facing programs sit on top of the driver's control interface and on SetupAPI:

```
tools/common/           pnctl.*   wrapper around the IOCTL interface, names, presets, config text round trip
                        pndrv.*   device nodes, driver store, per-device driver switching, services, signing, elevation
                        pnboot.*  boot settings, development-driver readiness and operation guard
                        pnspatial.* CLI-only Windows spatial compatibility and renderer discovery
                        pnlatency.* speaker-delay measurement: chirps, microphone recording, analysis (0.1.0.38)
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

Tabs appear in the order of the table below (application 0.1.0.34): Status,
Quality, Codecs, SBC, AAC, aptX, LDAC, Transport, Headset, Adapter, Driver. A device on
the Windows driver shows Audio, Quality and Driver. Each device opens on its first tab
(Status, or Audio in Windows mode); panels from 0.1.0.27 to 0.1.0.30 opened every
device on Quality.

When the panel starts, or the list is rebuilt with nothing selected, it selects the
receiver in use: the one whose output is the Windows default, then a streaming one, then
a connected one, then any PN-A2DP device. An existing selection is kept across refreshes.

| Tab | What it shows / changes |
|---|---|
| Status | Live connection state (an idle receiver with signalling up shows as **Connected**; the driver state is `PNA2DP_STATE_DISCOVERING`), codec, format, encoder owner, bitrate and ceiling, estimated latency, sink delay, MTU, AVDTP version, flags, last NTSTATUS (the format line names the Windows rate when it is converted; the sink delay names the delay applied and its source, for example "364 ms reported by the speaker", "315 ms fixed; the speaker reports 364 ms" or "200 ms default; the 25 ms report is ignored", and with drivers before 0.1.0.25 "reported by the headset" or "not reported; a per-codec default is assumed"); transport statistics (packets, bytes, drops, queue depth, write completion time, ABR level, codec host round trip); driver version, codec host service state, encoders available. Refreshes on every driver event (pended `WAIT_EVENT` IOCTL on a worker thread) and once a second. |
| Quality | Background audio checks with suggested settings, a live bitrate chart and recent transport counters, and a local diagnostic export that omits device identifiers. Also available in Windows mode with explicit telemetry limits. See [audio diagnostics](14-audio-diagnostics.md). |
| Codecs | Live codec/rate/bitrate, receiver support, live state, and saved or edited settings. Activate selected puts a supported codec first and applies it with SBC fallback. Auto quality + codecs restores automatic selection. The preset picker is a loader, not an indicator of saved settings. SBC cannot be disabled. |
| SBC | Max bitpool (slider, or the headset's maximum), min bitpool, bitpool override, dual channel preference, **SBC XQ** checkbox (dual channel, 16 blocks, 8 subbands at the bitpool plain dual channel gets, packed to the radio's EDR packets; it overrides the mode, block and subband choices), rate / channel mode / block / subband / allocation masks, live bitrate estimate at 44.1 and 48 kHz. |
| AAC | Target and peak bitrate or sink maximum, VBR, AAC-LC, afterburner, rate and channel masks. HE-AAC is not advertised. |
| aptX | Rate and channel masks for aptX, aptX HD (plus 16/24-bit input) and aptX LL (plus target buffer level). |
| LDAC | Quality (HQ / SQ / MQ / adaptive), ABR step-down, encoder input format, rate and channel masks, bitrate preview. |
| Transport | Listening profile (Music 150 / 450 ms, Balanced 60 / 250 ms, Low latency 30 / 120 ms, Custom; application 0.1.0.35, released in 0.1.0.36) above the target and maximum buffer sliders (maximum 40..500 ms), ABR on/off and floor, reconnect delay, persisted volume, behaviour flags (suspend on SCO, auto start, delay reports, expose all rates, software volume). |
| Headset | AVDTP version, SDP features, every remote SEP decoded (rates, channels, blocks, bitpool, AAC object types and bitrate, services) with the raw IEs. **Speaker delay (lip-sync)** (application 0.1.0.38, driver 0.1.0.25 or later): the delay applied and its source, **Automatic** / **Fixed** (1..1000 ms for the codec playing), a microphone choice and **Measure with microphone** ([below](#control-panel-01038--driver-01025-speaker-delay-released-as-v01038)). |
| Adapter | Actual radio name/address, Classic EDR capability, live health, effective bitrate range and observation counters. Auto quality + codecs detects receiver support and enables adapter-aware limits with live adaptation. |
| Driver | Driver, provider, version, INF, status, **Use PN-A2DP driver** / **Use Windows mode** / **Rescan**. Labelled Test Mode, Secure Boot and Memory Integrity lights; **Requirements...** explains blocked activation. **Windows Security** opens Core isolation; **Windows / Test Mode...** manages next-start Test Mode with confirmation and manual restart. Staged package with stage/remove, codec host service controls and installed plugins. |
| Audio (Windows mode) | Selected endpoint's volume, mute, format and output status, plus Windows Sound/Bluetooth settings links. |

The Spatial tab was removed in 0.1.0.24. Existing Windows spatial selections are
unaffected. Development-driver activation requires verified package files, actual
Test Mode ON, Secure Boot OFF and Memory Integrity OFF. The helper rechecks the
next-start boot mode too. Close the boot-settings dialog before switching drivers;
a shared operation guard prevents concurrent boot and driver changes.

Under the device list, **Connect** and **Disconnect** act on the selected receiver
whatever its driver (PN-A2DP 0.1.0.21 or later, or the Windows inbox driver). They send
the request Windows' own Connect / Disconnect send: a `KSPROPSETID_BtAudio`
`ONESHOT_RECONNECT` / `ONESHOT_DISCONNECT` GET on the topology filter behind the
receiver's audio endpoint (active, unplugged or disabled). The request runs off the UI
thread; the status line reports it, and the device list follows the result through
driver events, the per-second refresh and two follow-up reads after 2 s and 6 s, keeping
the selection and tab. Connect is enabled while no audio session is up, Disconnect while
a session or Bluetooth link is up. A PN-A2DP receiver with no Windows audio output at all
(every active and unplugged endpoint checked, none belongs to it) falls back to the
driver's reconnect, which resets the link, so only while the driver reports the session
idle or failed; an endpoint that could not be checked or reached is reported as such, not
as a missing output. A pressed button that disables itself hands keyboard focus to the
other button or to the device list. Demo mode only reports what would happen. The CLI has
the same commands: `pna2dpctl connect|disconnect [-d N]` for PN-A2DP devices and
`pna2dpctl windows connect|disconnect [-d N]` for any paired receiver.

The panel does not pair or remove receivers: pair a new receiver, or remove one, in
Windows Settings > Bluetooth & devices, then press **Refresh**. With no stereo receiver
paired, the status line points there.

Bottom bar: **Apply** sends the edited `PNA2DP_CONFIG` with `IOCTL_PNA2DP_SET_CONFIG`
(the driver validates, persists and applies live; bitpool changes go in band), **Revert**
reloads, **Defaults** loads the driver defaults (keeping the volume; the speaker delay
returns to Automatic), **Import / Export** round-trip the
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

Starting with application release **0.1.0.28**, the installer wizard, executable
version resources, and Apps & features registration all use the application
release version. The bundled signed kernel keeps its independent driver version
(0.1.0.21 in application release 0.1.0.34). Packaging rejects a stale control-panel binary instead of
labelling it as the new app release.

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

### Control panel 0.1.0.38 / driver 0.1.0.25: speaker delay, released as v0.1.0.38

Application 0.1.0.37 and driver 0.1.0.24 were intermediate builds; everything they contain
ships in [v0.1.0.38](releases/v0.1.0.38.md). Full record:
[speaker delay and lip-sync](21-speaker-delay-and-lip-sync.md).

- **Headset > Speaker delay (lip-sync).** A status line names the delay the driver applies
  and its source ("Using 364 ms reported by the speaker", "Using 315 ms, the fixed speaker
  delay; the speaker reports 364 ms", "Using 200 ms, the LDAC default; the speaker's report
  of 25 ms is ignored as impossible", "No stream"). **Automatic** uses the speaker's
  plausible reports, else the codec's default; **Fixed:** takes 1..1000 ms for the codec
  playing when it is chosen or typed (the label reads "ms, for LDAC"; it reads "ms, for any
  codec" only when Fixed was chosen while nothing played and no codec had been set before);
  an empty box starts from the delay in use. The receiver list
  and its detail are shorter to make room. With a driver before 0.1.0.25 the group is
  disabled and says that driver 0.1.0.25 or later is needed.
- **Microphone** lists the active recording devices, Windows' default marked "(default)"
  and selected first, read again when the list opens and on **Refresh**.
- **Measure with microphone** asks first (speaker, microphone, 8 short chirps over about
  13 seconds, "The recording is analysed in memory and not saved", 10 to 30 cm, other
  playback stopped, a quiet room; it adds a warning when Windows is already playing to the
  speaker, since the test needs a fresh stream). While it runs the button reads **Cancel
  measurement** and the text below shows the progress ("Starting the speaker...",
  "Waiting for the speaker to start playing...", "Chirp 3 of 8...", "Listening for the last
  chirp...", "Analysing the recording..."). A result such as "Measured 306 ms (8 of 8
  chirps, spread 10 ms). Apply changes to use it." becomes a pending **Fixed** value for the
  codec that played: **Apply changes** saves it ("Applied."), **Undo changes** drops it
  ("Not applied."). A failed run shows its reason (for example no microphone, a microphone
  recording below 16 kHz, only silence recorded, the speaker changing its delay during the
  test) and changes nothing. The run uses a worker thread and its own read-only driver
  handle; closing the panel or changing the device cancels it. Demo mode simulates the
  progress and a result without sound or devices.
- **Status > Sink delay** names the source in a short form: "364 ms reported by the
  speaker", "315 ms fixed; the speaker reports 364 ms", "200 ms default; the 25 ms report
  is ignored", "150 ms default; not reported", "no stream".
- Presets and **Auto quality + codecs** keep the speaker delay, like the volume; the
  footer's **Defaults** button resets it to Automatic.
- **Command line.** `pna2dpctl set sinkdelay=auto|<ms>` and
  `sinkdelay.codec=any|sbc|aac|aptx|aptxhd|aptxll|ldac`; `pna2dpctl config` prints
  `sink delay : 315 ms (LDAC)` or `automatic`; `pna2dpctl status` prints the estimate and a
  **sink delay** line with the source; `pna2dpctl measure-delay [-d N] [mic=<name part>]
  [apply]` runs the same measurement and, with `apply`, saves the result for the codec that
  played. Settings export and import carry `sinkdelay=` and `sinkdelay.codec=` (an edited
  line wins over the blob).
- **Receiver sample rates** (application 0.1.0.37). One decoder, `pn::SepRatesText`, serves
  the Headset page and `pna2dpctl caps`, which now prints a `rates` line for every codec
  (for the MR5 with its 96 kHz setting on: "rates 44.1k, 48k, 96k").
- `build/check-panel-text-fit.ps1` covers the new Status texts and the Headset page's
  speaker-delay texts (run it once on each page). On hardware, the Status page had 94
  texts and none cut off at the normal or minimum window size when idle; with four flags
  and a long Last error message, those two rows end in an ellipsis at the minimum size
  (end-ellipsis controls, by design).
- On hardware (EDIFIER MR5, a USB-C headset microphone) the panel's measurement ran to a
  result twice (306 and 316 ms). The owner's first attempt showed no result; this is not
  explained (the driver's last error then was 0xC00000B5, Device Timeout).

### Control panel 0.1.0.35: latency profiles and best-quality presets

- **Transport** has a **Listening profile** selector: Music (150 / 450 ms, rides out
  interference without lowering quality), Balanced (60 / 250 ms, the default), Low latency
  (30 / 120 ms, video and games) and Custom. Choosing a profile sets both buffer values;
  editing a value or moving a slider selects the matching profile or Custom. The maximum
  slider runs 40..500 ms, the driver's limit (it showed 1000), and the panel clamps to it.
- Presets match the new driver defaults (driver 0.1.0.22: LDAC HQ, AAC up to 320 kbps CBR,
  SBC at the receiver's maximum). **Best quality** keeps the receiver's maximum SBC bitpool
  (it forced 53) and uses Music buffering (it used 80 / 300 ms). **Balanced (driver
  defaults)** is now named **Driver defaults (best quality, balanced buffering)**; the preset
  list is wider so the name fits. **Low latency** uses the Low latency profile.
- Exported settings name the profile (`latency.profile=music`); `latency=` and
  `maxlatency=` are written only for Custom. `pna2dpctl set` accepts `latency.profile=` and
  `preset=best|defaults|lowlatency|compat|sbcxq`, and `pna2dpctl config` prints the profile.
  `preset=balanced` and `preset=low` are refused with a hint: they are latency profile words,
  and a preset replaces the whole configuration (codec order, SBC XQ / dual channel, auto
  start); use `latency.profile=` for buffering only (0.1.0.22 review #5).
- Existing devices keep their saved settings; Auto quality + codecs or a preset moves them
  to the new defaults. See [maximum audio quality](20-max-audio-quality.md).
- With driver 0.1.0.22 converting the sample rate, **Status** has a **Windows rate** row
  under **Format** ("48000 Hz, converted to 44100 Hz", or "44100 Hz, no conversion") and the
  flag **rate converted** (**rate converted (2:1)** for the 2:1 stage alone).
  `pna2dpctl status` puts the Windows rate on its format line ("44100 Hz (Windows 48000 Hz,
  converted), 2 ch, 32-bit in"), and the diagnostic export adds the Windows rate and the
  status flags.
- The first 0.1.0.36 build showed that combined line on the panel too, and it did not fit
  the value column: the end of the line was cut off (found by the owner on the MR5). The
  Windows rate now has its own row, and the Connection values are wider, so every Status
  value fits at the panel's minimum window size; `build/check-panel-text-fit.ps1` checks
  this against a running panel. The Driver line shows the four-part version (0.1.0.23). The SBC page note no longer says the Windows rate restricts
  the selection: a rate the receiver lacks is converted.

The selector, the wider preset list and the status line were built and checked in code
only; the panel was not run for this change during development. The selector and the
preset list on a real display remain an open check
([maximum audio quality](20-max-audio-quality.md#hardware-checks-still-to-do)).

### Control panel 0.1.0.36 / driver 0.1.0.23: released as v0.1.0.36

Application 0.1.0.35 and driver 0.1.0.22 were intermediate builds; everything above ships in
[v0.1.0.36](releases/v0.1.0.36.md) together with these changes:

- **Sink delay label.** **Status** shows the receiver's delay report whenever one arrives,
  "N ms reported by the headset", and otherwise "not reported; a per-codec default is
  assumed" (it showed "no delay reporting" and ignored a report sent without negotiation).
  `pna2dpctl status` prints "sink reports N ms" with "(reported without delay-reporting
  negotiation)" or "(not reported; a per-codec default is assumed)". Driver 0.1.0.23 uses
  such a report in the end-to-end estimate; the EDIFIER MR5's went from 209 to about
  401 ms.
- **Defaults.** A newly seen device starts at the driver's best-quality defaults (LDAC HQ,
  AAC up to 320 kbps CBR, SBC at the receiver's maximum, Balanced buffering). A device with
  saved settings keeps them; **Auto quality + codecs**, **Best quality** or **Driver
  defaults** moves it.
- **Windows format.** With driver 0.1.0.23, Windows uses the format chosen in **Sound >
  device > Properties > Advanced > Default Format** (earlier PN drivers made Windows'
  shared-mode engine use the driver's first listed format, normally 48 kHz). An application picks up a new format only when it reopens its stream;
  Chromium-based browsers keep theirs open while paused, so close and reopen them. A change
  during playback is bridged by the converter, and the **Windows rate** row on **Status**
  then shows, for example, "48000 Hz, converted to 44100 Hz".
- On hardware (EDIFIER MR5, driver 0.1.0.23): the converted format line, the delay report
  and the **Music** profile applied while playing were observed; see
  [maximum audio quality](20-max-audio-quality.md#hardware-results-14-october-2026). The
  profile was applied and the status read with `pna2dpctl`, so this does not count as a
  check of the Transport selector or the preset list.

### Control panels 0.1.0.31-0.1.0.34: Connect and Disconnect, tab order, SBC XQ

Released together as v0.1.0.34 with driver 0.1.0.21; see
[driver review fixes](18-driver-review-fixes.md).

- **Connect** and **Disconnect** buttons under the device list, and
  `pna2dpctl connect|disconnect` / `pna2dpctl windows connect|disconnect`, as described
  above. Driver 0.1.0.21 accepts the documented GET form of the one-shot properties
  (earlier drivers accepted only SET, so Windows Settings' own Connect and Disconnect
  were rejected for PN-A2DP endpoints). Owner-tested on hardware and reported working;
  there is no automated log.
- Tab order Status, Quality, Codecs, SBC, AAC, aptX, LDAC, Transport, Headset, Adapter,
  Driver (Windows mode: Audio, Quality, Driver); each device opens on its first tab.
- With no earlier selection the panel opens on the receiver in use (Windows default
  output, then streaming, then connected, then any PN-A2DP device).
- The driver state `PNA2DP_STATE_DISCOVERING`, reported whenever signalling is up
  without a configured stream, is shown as **Connected** in the panel, `pna2dpctl` and
  the diagnostic export.
- **SBC XQ** checkbox on the SBC page (`sbc.xq` in the configuration text). XQ never
  streams below plain dual channel: on receivers that advertise bitpool 53, such as the
  Sony and the MR5, it gives bitpool 53 = 672 kbps at 48 kHz (by design; the current
  XQ rule has not yet been played on a receiver). The SBC XQ preset sets it
  with the receiver's maximum bitpool and no override.
- `pna2dpctl trace` explains when it is denied for lack of elevation; `sbc_capture` also
  needs an elevated prompt, and codec host console mode must run as LocalSystem.

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
