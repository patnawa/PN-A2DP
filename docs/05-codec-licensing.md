# Codecs and licensing

Why the encoders live in a user-mode plugin host instead of the kernel driver:
the driver is MIT, the useful encoders are not, and some are patent-encumbered.
Loading them as separate DLLs through a C ABI keeps each component under its own
license and lets users or OEMs drop in a differently licensed encoder (for example a
Qualcomm-licensed aptX Adaptive library) without rebuilding anything.

| Codec | Implementation | License | Patents / trademarks | Where |
|---|---|---|---|---|
| SBC | `common/sbc` (written from the A2DP specification) | MIT | royalty free (Bluetooth SIG) | kernel + host (reference plugin) |
| AAC-LC / HE-AAC | Fraunhofer FDK AAC (`third_party/fdk-aac`) | FDK AAC license (BSD-like with patent clause) | core AAC-LC patents expired in most jurisdictions; Via LA still licenses HE-AAC | `pna2dp-codec-aac.dll` |
| aptX, aptX HD, aptX LL | libfreeaptx (`third_party/libfreeaptx`, fork of libopenaptx 0.2.0 / FFmpeg) | LGPL-2.1-or-later | the original aptX patent (US 9398620) is listed as expired; "aptX" is a Qualcomm trademark, so the UI must not claim Qualcomm certification | `pna2dp-codec-aptx.dll` + `freeaptx.dll` (LGPL replaceable) |
| LDAC | Sony libldac (`third_party/ldacBT`, AOSP) | Apache-2.0 (encoder only) | Sony licenses the trademark/certification for products; the open encoder is what Android and Linux ship | `pna2dp-codec-ldac.dll` |
| Opus (PipeWire A2DP profile) | libopus | BSD | royalty free | future plugin |
| LC3plus | Fraunhofer liblc3plus | proprietary | needs a license | not planned |
| aptX Adaptive / Lossless | Qualcomm only | proprietary | needs a license; Windows 11 24H2 supports it natively on Qualcomm radios | not possible for an open driver |
| LHDC | Savitech | proprietary | needs a license | not planned |

The Alternative A2DP Driver ships AOSP's SBC and libldac plus FDK AAC inside its kernel
binary; its aptX implementation is uncredited. PN-A2DP never links LGPL code into the
kernel image.

## Building the plugins

Each plugin has a small `build.ps1` that compiles the third-party library and the
adapter into `out\Release\pna2dp-codec-<name>.dll`:

```powershell
git submodule update --init --recursive
.\codec-host\plugins\ldac\build.ps1
.\codec-host\plugins\aptx\build.ps1
.\codec-host\plugins\aac\build.ps1
```

The host loads every `pna2dp-codec-*.dll` next to `pna2dp-codechost.exe`, in its
`plugins` sub-directory and in `%ProgramData%\PN-A2DP\plugins`. `pna2dpctl status`
shows which encoder is in use (`kernel` or `codec host`).

## Trademarks

"aptX" and "LDAC" are trademarks of Qualcomm and Sony. The project uses the names only
to identify the codec formats transported over A2DP, as Linux distributions do.
