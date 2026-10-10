# PN-A2DP

An open-source Bluetooth A2DP **source** driver for Windows 10/11 that replaces the
inbox `BthA2dp.sys` for a chosen headset and lets you pick and tune the codec:
SBC (XQ-style bitpools, dual channel), AAC, aptX, aptX HD, aptX Low Latency and LDAC,
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
```

## Layout

```
driver/pna2dp/       kernel driver (KMDF miniport + PortCls WaveRT), INF
common/sbc/          SBC encoder/decoder shared by kernel, host and tests (+ self test)
codec-host/          user-mode codec host service and plugins (SBC built-in; LDAC, aptX, AAC adapters)
tools/pna2dpctl/     command line control
build/               toolchain discovery, build, test signing, installation scripts
third_party/         codec libraries (git submodules; see docs/05)
```

## Build (no Visual Studio IDE needed)

```powershell
.\build\get-wdk.ps1            # once: downloads the WDK NuGet layout to %USERPROFILE%\wdk\c
.\build\build.ps1              # driver, codec host, control tool, SBC self test
.\out\Release\sbc_test.exe     # codec self test
```

Requires Visual Studio 2022 Build Tools with the C++ x64 toolset and the Windows SDK
10.0.26100.

## Install (development machines)

```powershell
.\build\build.ps1 -Sign                              # test-signs with a self-signed cert
bcdedit /set testsigning on ; shutdown /r /t 0        # elevated, once
.\build\install.ps1 -Package .\out\Release\package    # elevated: driver + codec host service
pna2dpctl list
pna2dpctl status
pna2dpctl set codecs=ldac,aac,sbc sbc.bitpool=76 sbc.dual=1
pna2dpctl watch
```

`.\build\install.ps1 -Uninstall` restores the inbox Microsoft driver.

## Status

The driver, the SBC codec, the codec host and the control tool compile from this tree.
The SBC codec is verified by a round-trip self test. The kernel driver has **not** yet
been loaded against real hardware from this environment (no kernel debugger or test
signing on the analysis PC); `docs/06-testing-plan.md` lists the bring-up steps.

## License

MIT for everything in this repository. Third-party codec libraries keep their own
licenses (Apache-2.0 for libldac, LGPL-2.1+ for libfreeaptx, Fraunhofer FDK AAC license).
