# Third-party notices

PN-A2DP itself (driver, codec host, control panel, installer, tools) is MIT licensed.
The optional codec plugins wrap third-party libraries that keep their own licenses.
Each plugin is a separate DLL loaded by the codec host service; none of this code is
linked into the kernel driver.

| Component | Library | License | Notes |
|---|---|---|---|
| `pna2dp-codec-ldac.dll` | Sony libldac (AOSP `ldacBT`) | Apache License 2.0 | Encoder only. "LDAC" is a trademark of Sony Corporation; the name is used to identify the codec format. |
| `pna2dp-codec-aptx.dll` + `freeaptx.dll` | libfreeaptx (fork of libopenaptx / FFmpeg aptX) | LGPL-2.1-or-later | `freeaptx.dll` is a separate, replaceable DLL as the LGPL requires. "aptX" is a trademark of Qualcomm Technologies; PN-A2DP is not certified by or affiliated with Qualcomm. |
| `pna2dp-codec-aac.dll` | Fraunhofer FDK AAC | FDK AAC license (BSD-style with patent clause) | See `third_party/fdk-aac/NOTICE`. |
| `pna2dp.sys`, `pna2dp-codechost.exe` (built-in SBC) | PN-A2DP `common/sbc` | MIT | Written from the Bluetooth A2DP specification. |

The full license texts are in the respective `third_party/` submodules of the source
tree (https://github.com/patnawa/PN-A2DP) and are installed next to each plugin when the
installer places them.
