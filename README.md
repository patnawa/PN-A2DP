<div align="center">

# PN-A2DP

**Bluetooth audio, tuned for you.**

A Windows Bluetooth audio control panel with automatic codec selection,
adaptive quality, and diagnostics you can understand.

**Windows 10 / 11 · x64 · Control panel v0.1.0.30 · MIT project license**

[Get started](#get-started) · [Choose a mode](#choose-a-mode) · [Codec support](#codec-support) · [Documentation](#documentation)

</div>

![PN-A2DP Quality dashboard showing an active Sony SBC stream, a 60-second bitrate graph, and audio health checks](docs/images/quality-dashboard.jpg)

*Actual control panel v0.1.0.27 on Windows 11, streaming to a Sony CMT-SBT40D.
The dashboard reports observed transport health; it does not measure acoustic quality.*

## What you can do

- **Check my audio.** Inspect the selected device, its Windows output, volume,
  mute, format, and available codec information without changing your settings.
- **See live quality.** In PN driver mode, follow a rolling 60-second bitrate
  graph, packet progress, drops, write failures, and automatic quality recovery.
- **Let compatibility guide the codec.** Detect receiver support, match it with
  available encoders, and choose automatically or set your own priorities.
- **Start high and adapt.** Automatic quality respects the receiver's limits and
  your configured ceilings, reducing supported variable bitrates during sustained
  congestion and recovering after healthy traffic.
- **Switch per device.** Use the Windows Bluetooth audio driver or the PN-A2DP
  driver for each paired receiver, with readiness checks and clear restart status.
- **Export useful diagnostics.** Save a local text report that omits device names,
  Bluetooth addresses, endpoint IDs, user paths, and raw logs. No audio is recorded
  and no report is uploaded automatically.

## Choose a mode

| Capability | Windows mode | PN driver mode |
|---|---|---|
| Bluetooth audio driver | Microsoft's built-in driver | PN-A2DP custom source driver |
| Output volume, mute, and Windows mix format | Available in the Audio tab | Integrated with Windows audio controls |
| Check my audio and diagnostic export | Device and output checks | Adds codec, encoder, service, and transport checks |
| Codec selection and tuning | Managed by Windows | SBC, AAC-LC, aptX family, and LDAC, subject to receiver support |
| Live Bluetooth bitrate and packet health | Not exposed by this backend | Available in the Quality tab |
| Automatic quality management | Managed by Windows | Receiver-aware policy for supported variable-rate codecs |
| Custom kernel driver required | No | Yes |
| Current installation path | Application-only installer or portable panel | Development package on a prepared test machine |

**Start with Windows mode** for device controls and diagnostics without installing
a custom driver. It needs no Test Mode, driver certificate, or Microsoft developer
account. Installing this package preserves existing device driver assignments;
use **Driver → Use Windows mode** to move a device off the PN driver.

**PN driver mode is currently a development build.** The validated configuration
requires Test Mode on, Secure Boot off, and Memory Integrity off. The panel checks
these requirements before activation. Normal-mode distribution of the custom
driver requires a Microsoft-signed driver package; accepting a warning or adding
a self-signed certificate does not replace that requirement. See the
[build and installation guide](docs/04-build-sign-install.md).

## Get started

There are currently no packaged GitHub releases. Build from source using
**Visual Studio 2022 Build Tools** with the C++ x64 toolset and
**Windows SDK 10.0.26100**. The scripts discover the toolchain; the Visual Studio
IDE is not required.

```powershell
git clone --recurse-submodules https://github.com/patnawa/PN-A2DP.git
cd PN-A2DP
.\build\get-wdk.ps1
.\build\build.ps1 -Target windows -Config Release
```

Choose one of the generated Windows-mode packages:

| Package | Output |
|---|---|
| Installer | `out/Release/dist-windows/pna2dp-setup.exe` |
| Portable control panel | `out/Release/dist-windows/portable/pna2dp-panel.exe` |

Pair your speaker or headphones in Windows, open the panel, select the device,
and press **Check my audio** on the **Quality** tab. The **Audio** tab provides
volume and mute controls for devices using the Windows driver.

To explore the interface with simulated devices:

```powershell
.\out\Release\pna2dp-panel.exe --demo
```

Demo mode is visibly labelled and does not demonstrate physical receiver support.
For switching, installation details, and limitations, see [Windows mode](docs/13-windows-mode.md).

## Codec support

These controls apply to **PN driver mode**. A codec must be supported by both
the receiver and an available encoder. Windows mode uses Windows' own selection.

| Codec | Implementation and controls | Current validation |
|---|---|---|
| **SBC** | Built-in encoder and kernel fallback; receiver-limited bitpool and channel modes | Software tests, independent decoding, and physical Sony playback |
| **AAC-LC** | FDK AAC plugin; bitrate controls and adaptation | Software tests and independent decoding |
| **aptX / aptX HD** | libfreeaptx plugin; fixed codec bitrates | Software tests and independent decoding |
| **aptX Low Latency** | aptX encoder with LL negotiation and transport support | Software checks; receiver interoperability and latency remain unverified |
| **LDAC** | Sony libldac plugin; quality levels and adaptive bitrate | Encoder and parameter tests; independent decoding and physical receiver validation remain outstanding |

Automatic quality uses measured transport behavior as well as reported adapter
capabilities. Bluetooth version and EDR support alone do not establish usable
bandwidth. Fixed-rate aptX codecs cannot be reduced in-band. The driver does not
resample: the Windows audio engine rate must be supported by the selected codec
and receiver. AAC support is LC; HE-AAC is not advertised.

Read more about [automatic adapter quality](docs/10-adapter-quality.md) and
[codec licensing](docs/05-codec-licensing.md).

Redistribution bundles include full codec notices and corresponding source
archives. This does not establish patent clearance or product certification.
Before publishing free binaries or selling the program, see the
[release-readiness audit and costs](docs/15-commercial-readiness.md).

## Build and verify the full project

Build the driver, codec host, encoder plugins, tools, and installer:

```powershell
.\build\build.ps1 -Target package -Config Release
.\build\verify.ps1 -SkipBuild -Sanitize -Interop
```

The full installer is generated at `out/Release/dist/pna2dp-setup.exe`.
Building a package does not prepare Windows to load a development driver.
Follow the [signing and installation guide](docs/04-build-sign-install.md) before
attempting PN driver activation. The optional `-Interop` checks require Python
and FFmpeg on `PATH`.

The verification suite covers codec negotiation, AVDTP parsing and reassembly,
PCM conversion, RTP, configuration handling, diagnostics, service operations,
Windows-mode behavior, and installer signing checks. It includes AddressSanitizer
runs, 50,000 malformed packet inputs, and 316 plugin parameter combinations.

**Hardware tested:** Windows 11 build 26300, Qualcomm FastConnect 7800, and Sony
CMT-SBT40D using SBC. Volume, mute, and audible playback have been checked on this
setup. Other codec receivers, Driver Verifier/HLK, and broader power/PnP stress
coverage remain work in progress. See the [test record and remaining coverage](docs/06-testing-plan.md).

<details>
<summary><strong>Repository layout</strong></summary>

```text
driver/pna2dp/       KMDF / PortCls WaveRT driver and INF
common/sbc/         Shared SBC encoder, decoder, and tests
codec-host/         User-mode codec host and encoder plugins
tools/pna2dp-panel/ Native Windows control panel
tools/pna2dpctl/    Command-line diagnostics and configuration
tools/pna2dp-setup/ Installer and uninstaller
tools/common/      Shared device, audio, driver, and diagnostic helpers
build/             Toolchain, build, packaging, and verification scripts
third_party/       Codec library submodules and their licenses
docs/              Architecture, installation, and validation notes
```

</details>

## Documentation

| Guide | Contents |
|---|---|
| [Audio diagnostics](docs/14-audio-diagnostics.md) | Check my audio, live quality, export contents, and privacy |
| [Windows mode](docs/13-windows-mode.md) | Standard driver controls, switching, and boot-mode readiness |
| [Automatic quality](docs/10-adapter-quality.md) | Adapter detection, congestion response, and recovery |
| [Control panel and installer](docs/07-control-panel-and-installer.md) | Device controls, configuration, and installation behavior |
| [Build, sign, and install](docs/04-build-sign-install.md) | Development setup and driver deployment |
| [Production signing — ภาษาไทย](docs/11-production-signing-th.md) | Microsoft signing requirements and submission preparation |
| [Architecture](docs/03-architecture.md) | Driver, codec host, plugins, and control interfaces |
| [Testing](docs/06-testing-plan.md) | Reproducible checks, hardware results, and open coverage |

## Contributing

For a bug report, include the panel version, driver mode, receiver model, steps to
reproduce, and expected versus actual behavior. A report from **Quality → Export
diagnostics** can help; review it before sharing. For code changes, run the relevant
checks and state which codecs and hardware you actually tested.

## License

PN-A2DP's own code is licensed under [MIT](LICENSE). Third-party codec libraries
retain their respective licenses: Apache-2.0 for libldac, LGPL-2.1-or-later for
libfreeaptx, and the Fraunhofer FDK AAC license. See
[third-party notices](THIRD-PARTY-NOTICES.md) and the
[codec licensing guide](docs/05-codec-licensing.md) before redistribution.
