# PN-A2DP

An open-source Bluetooth A2DP **source** driver for Windows 10/11 that replaces the
inbox `BthA2dp.sys` for a chosen headset and lets you pick and tune the codec:
SBC (XQ-style bitpools, dual channel), AAC-LC, aptX, aptX HD, aptX Low Latency and LDAC,
with live reconfiguration, adaptive bitrate, latency reporting and an open plugin
host for encoders.

It was designed after a deep teardown of the commercial "Alternative A2DP Driver"
(Bluetooth Goodies) and of the Windows Bluetooth audio stack; see `docs/`.

```
docs/01-alternative-a2dp-analysis.md       what the commercial driver does and how
docs/02-windows-bluetooth-audio-internals.md how Windows loads, enumerates and drives A2DP
docs/03-architecture.md                    PN-A2DP design and why it is better
docs/04-build-sign-install.md              building, signing, installing, debugging
docs/05-codec-licensing.md                 which encoder may be shipped where
docs/06-testing-plan.md                    what has been verified and what has not
docs/07-control-panel-and-installer.md     the GUI, per-device driver switching and the installer
docs/09-spatial-audio.md                   Dolby/DTS headphone compatibility, Windows detection and transport limits
```

## Layout

```
driver/pna2dp/       kernel driver (KMDF miniport + PortCls WaveRT), INF
common/sbc/          SBC encoder/decoder shared by kernel, host and tests (+ self test)
codec-host/          user-mode codec host service and plugins (SBC built-in; LDAC, aptX, AAC adapters)
tools/pna2dpctl/     command line control
tools/pna2dp-panel/  control panel (codec priority, per-codec tuning, latency, live status, driver switching)
tools/pna2dp-setup/  installer / uninstaller (single exe, embeds driver, service, plugins, tools)
tools/common/        shared user-mode code: IOCTL wrapper, SetupAPI driver management, services, signing
build/               toolchain discovery, build, test signing, installation and packaging scripts
third_party/         codec libraries (git submodules; see docs/05)
```

## Build (no Visual Studio IDE needed)

```powershell
.\build\get-wdk.ps1            # once: downloads the WDK NuGet layout to %USERPROFILE%\wdk\c
.\build\build.ps1              # driver, codec host, control tool, control panel, SBC self test
.\out\Release\sbc_test.exe     # codec self test
.\build\build.ps1 -Target package   # + plugins + installer -> out\Release\dist\pna2dp-setup.exe
.\build\verify.ps1 -Sign -Sanitize -Interop   # reproducible software verification
.\out\Release\pna2dp-panel.exe --demo   # control panel with a simulated headset
```

Requires Visual Studio 2022 Build Tools with the C++ x64 toolset and the Windows SDK
10.0.26100. The optional `-Interop` checks also need Python and FFmpeg in PATH.

## Windows mode (no custom driver)

Control panel 0.1.0.21 also works with the Microsoft Bluetooth audio driver.
Use `out/Release/dist-windows/pna2dp-setup.exe` or its portable panel. This package
contains only application tools and needs no Test Mode or custom driver certificate.
Select **Driver > Use Windows mode** for a device currently using PN-A2DP; restart
manually if Windows requests it. Devices on the Windows driver show output volume,
mute and format. Windows manages their codec and bitrate; the
advanced codec controls remain available for devices on the PN-A2DP driver.

Build with `.\build\build.ps1 -Target windows`. See [Windows mode](docs/13-windows-mode.md)
for capabilities, limitations, switching, and tests.

## Install

Run `pna2dp-setup.exe` (from `out\Release\dist` or a release). The wizard lets you pick
the components, the paired headsets that should use PN-A2DP (the others keep the
Microsoft driver), and verifies Microsoft driver signatures for normal-mode installation. It does
not enable Test Mode or install test certificates. It registers in Apps & features; `uninstall.exe` restores the inbox driver on
every headset. `pna2dp-setup.exe --silent` installs unattended.

Afterwards use the **PN-A2DP Control Panel** (Start menu) to switch headsets between the
drivers, order the codecs, tune SBC / AAC / aptX / LDAC, set the buffer size and watch
the live status, or the command line tool:

```powershell
pna2dpctl list
pna2dpctl status
pna2dpctl spatial
pna2dpctl set codecs=ldac,aac,sbc sbc.bitpool=76 sbc.dual=1
pna2dpctl watch
```

Script-based alternative for development machines:

```powershell
.\build\build.ps1 -Sign                              # test-signs with a self-signed cert
bcdedit /set testsigning on                          # elevated; restart manually when ready
.\build\install.ps1 -Package .\out\Release\package -AllowTestDriver  # elevated; prepared development PC
```

For normal-mode releases, see the [account and Microsoft signing guide](docs/11-production-signing-th.md).
The current local driver remains a development build. Developer setup requires
an already prepared test machine and the explicit `--allow-test-driver` option
(or `-AllowTestDriver` with `install.ps1`).

`.\build\install.ps1 -Uninstall` restores the inbox Microsoft driver.

## Status

Development status: the x64 driver, codec host, tools, installer and three external
codec plugins build locally. The test suite covers production codec negotiation,
AVDTP parsing/reassembly, PCM conversion and RTP, with 50,000 malformed packet inputs
under AddressSanitizer. All 316 plugin parameter combinations pass. SBC configurations
and full-scale bass/transient regressions also decode with FFmpeg's independent decoder.

Real SBC music playback has been tested on Windows 11 build 26300 with a Sony
CMT-SBT40D and Qualcomm FastConnect 7800. The user confirmed normal loudness and
working Windows volume/mute. Windows receiver-volume integration avoids double
attenuation, with software gain as a fallback. Version 0.1.0.13 also fixes an SBC
joint-stereo overflow on loud bass transients and retains PCM during temporary
Bluetooth write congestion. Endpoint icons follow the receiver's Bluetooth device class.
Use **Automatic** in the panel or `pna2dpctl auto` to negotiate supported codecs
without manually enabling incompatible receiver modes. It starts LDAC at HQ
(990/909 kbps) or AAC-LC at up to 320 kbps, respects receiver limits, and reduces
bitrate if the link becomes congested. Version 0.1.0.14 fixes LDAC's ABR quality
ceiling and honors its per-codec ABR switch.
Version 0.1.0.16 adds the **Adapter** tab and `pna2dpctl quality`: it identifies
this receiver's actual radio, reports Classic EDR capabilities and measured transport
health, and automatically adapts supported variable-bitrate codecs. Use **Auto quality
+ codecs** to start at the highest supported settings. See [automatic adapter quality](docs/10-adapter-quality.md).
Control panel 0.1.0.19 adds a resizable layout, device cards with readable state
and codec information, a live output summary on every tab, and clear Apply/Undo
actions with an unsaved-changes indicator.
Version 0.1.0.18 makes automatic preferences explicit in the SBC, AAC,
aptX and LDAC tabs, reads the SBC maximum from the receiver and shows live codec
status alongside saved preferences. Version 0.1.0.17 added **Activate selected** to the Codecs tab and shows the
live codec, receiver compatibility and actual saved settings. The preset menu now
clearly previews settings to load instead of always showing Balanced.
Version **0.1.0.24** removes the Spatial tab and adds **Driver** readiness lights
for Test Mode, Secure Boot and Memory Integrity. Development-driver activation is
blocked until its requirements pass; **Requirements...** explains any blockers.
Close the boot-settings window before switching drivers and restart manually
when a setting requires it. The program verifies the package and platform again
in the elevated activation helper. See [Windows mode](docs/13-windows-mode.md).
Windows spatial sound is managed in Windows Sound settings; the CLI retains
read-only discovery. See [spatial audio](docs/09-spatial-audio.md) for transport limits.
See [verification details and remaining hardware coverage](docs/06-testing-plan.md).
The driver does not resample: the audio engine's rate must be supported by the selected
codec and sink. AAC supports LC; HE-AAC is not advertised. aptX LL transport and latency
remain unverified with an actual aptX LL receiver.

## License

MIT for everything in this repository. Third-party codec libraries keep their own
licenses (Apache-2.0 for libldac, LGPL-2.1+ for libfreeaptx, Fraunhofer FDK AAC license).
