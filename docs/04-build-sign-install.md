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
sbc_test.exe                     SBC self test
```

## Signing

Windows loads kernel drivers only when they are signed by Microsoft:

* **Attestation signing** (Partner Center, Hardware Dev Center account + EV code signing
  certificate): sign the `.cab` of the package, upload, download the signed package.
  No HLK tests are required for attestation; the driver loads on Windows 10 and 11
  with Secure Boot enabled. This is what the Alternative A2DP Driver ships.
* **WHQL / HLK**: full logo tests (`Audio`, `Bluetooth` and `Device.Streaming` HLK
  suites) give a WHQL signature that also satisfies Windows Server and lets Windows
  Update distribute the driver.
* **Test signing** (development only): `build\sign-test.ps1` creates a self-signed
  certificate, builds the catalog with `inf2cat` and signs `.sys` and `.cat`. The
  machine must run with `bcdedit /set testsigning on` and the certificate must be in
  the Root and TrustedPublisher stores.

Secure Boot must be disabled for test signing on most firmware; HVCI (Memory
integrity) does not block attestation-signed drivers but does block test-signed ones
unless the test root is deployed through `bcdedit /set nointegritychecks`-free means,
so disable memory integrity on the development machine.

## Installing

```powershell
.\build\install.ps1 -Package .\out\Release\package      # elevated
```

1. `pnputil /add-driver` stages the INF in the driver store.
2. Because the inbox `microsoft_bluetooth_a2dp_src.inf` matches the same hardware ID
   with a Microsoft signature and `PreConfigureDriver`, Windows will not pick ours by
   rank. `update-device-driver.ps1` therefore calls `DiInstallDevice` with
   `DIIRFLAG_FORCE_INF` for each `BTHENUM\{0000110b-...}` node, exactly like Device
   Manager's "Have Disk" update and like the commercial installer.
3. The codec host service is copied to `%ProgramFiles%\PN-A2DP` and registered as an
   auto-start service.

Windows Update can later re-install the inbox driver; run the script again. A future
version will add a watchdog (the commercial product's `AltA2dpSVC` does this).

`.\build\install.ps1 -Uninstall` removes the driver package; Windows re-installs the
inbox driver on the next scan.

## Debugging

* `DebugLevel` (REG_DWORD) under `HKLM\SYSTEM\CurrentControlSet\Services\pna2dp\Parameters`:
  1 errors, 2 warnings, 3 info (default), 4 verbose. Output goes to `DbgPrintEx`
  (DPFLTR_IHVAUDIO_ID), visible in WinDbg or DebugView with kernel capture enabled.
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
