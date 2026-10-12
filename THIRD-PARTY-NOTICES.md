# Third-party notices

PN-A2DP itself (driver, codec host, control panel, installer, tools) is MIT licensed.
The optional codec plugins wrap third-party libraries that keep their own licenses.
Each plugin is a separate DLL loaded by the codec host service; none of this code is
linked into the kernel driver.

| Component | Library | License | Notes |
|---|---|---|---|
| `pna2dp-codec-ldac.dll` | Sony libldac (AOSP `ldacBT`) | Apache License 2.0 | Encoder only. The upstream certification notice is preserved. "LDAC" is a Sony trademark; no certification or endorsement is claimed. |
| `pna2dp-codec-aptx.dll` + `freeaptx.dll` | libfreeaptx (fork of libopenaptx / FFmpeg aptX) | LGPL-2.1-or-later | `freeaptx.dll` remains separately replaceable with a compatible modified library. "aptX" is a Qualcomm trademark; PN-A2DP is not certified by or affiliated with Qualcomm. |
| `pna2dp-codec-aac.dll` | Fraunhofer FDK AAC | FDK AAC software license | PN-A2DP negotiates AAC-LC only. The library license does not grant patent rights. |
| `pna2dp.sys`, `pna2dp-codechost.exe` (built-in SBC) | PN-A2DP `common/sbc` | MIT | Written from the Bluetooth A2DP specification. |

For each included codec, the installer and portable package carry these files:

| Codec | Full license / notice | Corresponding source and build information |
|---|---|---|
| AAC | `LICENSE-FDK-AAC.txt` | `sources-aac.zip`, `sources-aac.json` |
| aptX family | `LICENSE-LGPL-2.1.txt` | `sources-aptx.zip`, `sources-aptx.json` |
| LDAC | `LICENSE-LDAC.txt`, `NOTICE-LDAC.txt` | `sources-ldac.zip`, `sources-ldac.json` |

The source archives are included without a separate fee and do not need a GitHub
account or access to a private repository. They contain the actual library source,
PN adapter, headers, build scripts, full upstream notices and build instructions.
The JSON manifests identify the upstream revision and SHA-256 hashes of the source
archive, notices and corresponding binaries. Original upstream headers and notices
are retained. Packaging checks these artifacts; this is not legal certification.

Recipients retain all rights granted by each component's license. In particular,
PN-A2DP imposes no additional restriction on modification of the LGPL library or
reverse engineering for debugging modifications to it. Commercial terms, if added
later, must not override those rights. Do not charge copyright-license fees for
use, copying or distribution of the FDK AAC library or modifications to it.

Windows-mode packages contain no external codec binaries and therefore do not
bundle these codec source archives. The core MIT license still accompanies them.

Copyright permissions, patent rights, Bluetooth qualification and codec trademarks
are separate matters. Neither a free download nor these notices establish patent
clearance, Sony/Qualcomm certification, or permission to use their logos.
