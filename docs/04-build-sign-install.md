# Building, signing, installing and debugging

## Toolchain

| Component | Where it comes from |
|---|---|
| MSVC x64 (14.44) | Visual Studio 2022 Build Tools, "Desktop development with C++" |
| Windows SDK 10.0.26100 | Visual Studio installer or winget `Microsoft.WindowsSDK.10.0.26100` |
| WDK 10.0.26100 | `build\get-wdk.ps1` downloads the official NuGet package `Microsoft.Windows.WDK.x64` and extracts it to `%USERPROFILE%\wdk\c` (no admin rights). An installed WDK under `Program Files (x86)\Windows Kits\10` is detected too. |

`build\toolchain.ps1` locates everything and exports the include/lib paths;
`build\build.ps1` calls `cl`, `rc` and `link` directly with the flags the WDK's MSBuild
targets would use (`/kernel`, `/GS`, `/guard:cf`, `/DRIVER`, `/ENTRY:FxDriverEntry`,
KMDF 1.31 so Windows 10 21H2+ and Windows 11 can load the binary). No WDK Visual
Studio extension is needed, which keeps the build reproducible on a bare Build Tools
install and in CI.

```powershell
.\build\build.ps1                 # Release: driver + host + cli + tests
.\build\build.ps1 -Config Debug
.\build\build.ps1 -Target driver -Verbose
```

Outputs land in `out\<Config>`:

```
pna2dp.sys / pna2dp.pdb          kernel driver
package\pna2dp.inf + pna2dp.sys  install package (+ .cat and test cert after -Sign)
pna2dp-codechost.exe             codec host service
pna2dpctl.exe                    control tool
pna2dp-panel.exe                 control panel (-Target gui)
pna2dp-setup.exe                 installer with everything above embedded (-Target setup)
dist\                            installer + portable zip (-Target package)
sbc_test.exe                     SBC self test
quality_test.exe                 end-to-end audio-quality harness (-Target quality-tests)
```

### Build checks (driver 0.1.0.19 and later)

* The driver compiles with `/W4 /WX` and `/Qspectre` and links with `/CETCOMPAT`. It
  builds with zero warnings in Release and Debug.
* Every driver build runs ApiValidator against the WDK's universal DDI list and InfVerif
  on the stamped INF; either failure fails the build.
* `build\check-analyze.ps1` runs the Debug `-Analyze` driver build and fails when the
  number of unique warnings rises above `build\analyze-baseline.txt` (currently 23, all
  in SDK/WDK headers). After removing warnings, lower the baseline with
  `-UpdateBaseline`; never raise it to hide a new one. Only the count is compared.
* `build.ps1 -Target plugins` builds the codec plugins alone; `-Target quality-tests`
  builds `quality_test.exe`, which `build\test-audio-quality.py` runs with FFmpeg and
  the plugins to measure every codec path (decoded by FFmpeg where it has a decoder; not
  LDAC) and flag clicks, gaps, clipping and accounting errors.
* `build\verify.ps1` adds the `driver-analysis` and `api-validator` checks, and with
  `-Interop` also `build-audio-quality` and `audio-quality`. The full
  `verify.ps1 -Sign -Sanitize -Interop` run for v0.1.0.34 passed 34 checks.
* `.github/workflows/verify.yml` builds the tests, driver, host, CLI, panel and plugins
  unsigned on a hosted `windows-2022` runner and runs InfVerif, ApiValidator, the pure
  unit tests and the analysis ratchet (advisory). It never signs, installs or opens a
  device. It is **manual-only** (`workflow_dispatch`) until it has passed once, and it
  **has not run yet**.

## Signing

For normal-mode installation, new kernel drivers need a Microsoft driver
signature from the Hardware Developer Program. An EV certificate on its own,
a trusted self-signed certificate, or an Authenticode signature on Setup does
not replace Microsoft's kernel-driver signature.

* **WHCP / HLK** is the recommended retail release path. Run the applicable HLK
  tests and submit the signed HLK package through Partner Center.
* **Attestation** is documented by Microsoft for testing scenarios on Windows
  Desktop; it does not certify HLK compliance or permit retail Windows Update
  publication. Check current program requirements before choosing it.
* **Test signing** is for separately prepared development machines. Setup no
  longer imports test roots or changes boot policy. `--allow-test-driver` must
  be explicitly supplied, the package must already be trusted, and test signing
  must already be active.

See [the Thai account/submission/release guide](11-production-signing-th.md),
[Microsoft's signing options](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)
and [registration requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/hardware-program-register).
Memory integrity requires compatible driver code as well as acceptable signing;
a signature alone is not evidence that the driver passed HVCI testing.

Use `build/prepare-submission.ps1` to prepare a CAB and hash manifest without
changing the machine. After Microsoft returns the signed INF/SYS/CAT, package
those exact bytes with:

```powershell
.\build\build.ps1 -Target dist -Production -DriverPackage 'C:\SignedDriver\PN-A2DP'
```

Production packaging rejects test certificates, checks Microsoft driver signing
and file membership, and runs SignTool kernel-policy verification before updating
`dist`. Do not rebuild or edit the returned driver package after signing.

## Installing

End users run `pna2dp-setup.exe` (`build.ps1 -Target package` produces it in
`out\Release\dist`); it performs the steps below with a wizard, lets the user choose which
headsets get the driver, and registers an uninstaller. See `07-control-panel-and-installer.md`.
The script below is the equivalent for development machines:

```powershell
.\build\install.ps1 -Package .\out\Release\package -AllowTestDriver   # elevated; prepared development PC
```

1. `pnputil /add-driver` stages the INF in the driver store.
2. The inbox `microsoft_bluetooth_a2dp_src.inf` matches the same hardware ID with a
   Microsoft signature and `PreConfigureDriver`, and receivers already on the inbox
   driver are not moved to ours by rank alone. `update-device-driver.ps1` therefore
   calls `DiInstallDevice` with
   `DIIRFLAG_FORCE_INF` for each `BTHENUM\{0000110b-...}` node, exactly like Device
   Manager's "Have Disk" update and like the commercial installer.
3. The codec host service is copied to `%ProgramFiles%\PN-A2DP` and registered as an
   auto-start service.

Observed on Windows 11 build 26300 with driver 0.1.0.20: once the PN-A2DP package is
in the driver store, a receiver paired **afterwards** bound to PN-A2DP automatically,
because its INF date outranks Microsoft's. Use **Driver > Use Windows mode** in the panel
for a receiver that should stay on the inbox driver.

Stop playback before installing or updating the driver. While an audio stream is open,
the device is left pending a restart and `pnputil` cannot restart it, so the old driver
stays loaded until Windows restarts.

Windows Update can later re-install the inbox driver; run the script again. A future
version will add a watchdog (the commercial product's `AltA2dpSVC` does this).

`.\build\install.ps1 -Uninstall` removes the driver package; Windows re-installs the
inbox driver on the next scan.

### First load on a development machine

Because the BTHENUM node of a paired headset exists at every boot, a driver that crashes
while starting would crash every boot. Three layers protect against that:

1. `build\switch-device.ps1` (elevated, after the test-signing restart) switches one
   device: it checks that test signing is active and the certificate trusted, creates a
   System Restore point, records the previous driver in
   `%ProgramData%\PN-A2DP\previous-driver.txt` and reports the result.
   `switch-device.ps1 -Rollback` reinstalls the recorded driver.
2. Crash guard: `DriverEntry` counts every load in
   `HKLM\SYSTEM\CurrentControlSet\Services\pna2dp\State\UncleanStarts`; a clean shutdown or
   restart, a driver unload, or 10 minutes of uptime reset it. After two loads in a row
   without one of those, the driver refuses to load (the device shows Code 37) and
   Windows boots. Switching a device to PN-A2DP (`switch-device.ps1`, the installer or the
   control panel) clears the value; delete it by hand to try again.
3. If Windows still cannot start: after two failed boots it opens the recovery
   environment; Troubleshoot > Advanced options > Command Prompt,
   `bcdedit /set {default} testsigning off`, restart. Safe Mode also works because the
   Bluetooth stack does not start there.

## Debugging

* `DebugLevel` (REG_DWORD) under `HKLM\SYSTEM\CurrentControlSet\Services\pna2dp\Parameters`:
  1 errors, 2 warnings (default since driver 0.1.0.19), 3 info, 4 verbose. Output goes
  to `DbgPrintEx` (DPFLTR_IHVAUDIO_ID), visible in WinDbg or DebugView with kernel
  capture enabled. The trace ring (`pna2dpctl trace`, elevated prompt required) keeps
  errors, warnings and information at every level; verbose messages only at 4.
* TraceLogging provider `PnA2dp.Driver` and System event log source `pna2dp`; see
  [driver review fixes](18-driver-review-fixes.md#security-and-observability).
* `pna2dpctl watch` shows every state change with the current codec, bitrate and
  latency estimate; `pna2dpctl stats` shows transport counters (drops, queue depth,
  completion latency, ABR level, host round trip).
* `pna2dpctl caps` dumps the sink's SEPs and raw codec IEs; compare with the Windows
  inbox driver using `btsnoop`/Wireshark on the HCI log (Windows supports HCI logging
  with the Bluetooth virtual sniffer, `logman`/`BluetoothVirtualSniffer`).
* Pool tags: `PnDv` device, `PnL2` L2CAP buffers, `PnAv` AVDTP, `PnPc` PCM, `PnCd`
  codec results, `PnAu` audio objects.

## Build matrix

| Target | Status in this tree |
|---|---|
| x64 Release/Debug | builds |
| arm64 | INF section present; toolchain script currently targets x64 only (`Microsoft.Windows.WDK.arm64` NuGet + `Hostx64\arm64` cl) |
