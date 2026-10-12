# Codecs and redistribution

Reviewed 2026-10-12. This records source licenses and packaging, not a patent
opinion or approval to distribute a Bluetooth product. The same questions apply
to a free GitHub binary release.

PN-A2DP's own code is MIT licensed. Optional encoders run in user-mode DLLs;
their individual licenses still apply. A plugin boundary alone does not discharge
source, notice, patent or trademark obligations.

| Component | Source and copyright license | Release considerations |
|---|---|---|
| SBC | Project `common/sbc`, MIT | Retain the project license. Bluetooth specification/IP and qualification requirements need separate assessment; this is not a blanket royalty-clearance claim. |
| AAC-LC | `third_party/fdk-aac`, FDK AAC software license | Retain the full license and provide the complete library source and modifications free of charge to binary recipients. The license grants no patent license. PN-A2DP advertises AAC-LC, not HE-AAC. |
| aptX / HD / LL | `third_party/libfreeaptx`, LGPL-2.1-or-later | Ship the license and corresponding source/build information. Keep `freeaptx.dll` replaceable with a compatible modified library and preserve applicable LGPL modification/debugging rights. Patent and branding status needs separate review. |
| LDAC | `third_party/ldacBT`, including `libldac`, Apache-2.0 | Retain the Apache license and upstream NOTICE. The NOTICE says product use requires certification; ask Sony how it applies to this Windows encoder before public distribution. No certification or endorsement is claimed. |

The controlling source texts are the [FDK license](../third_party/fdk-aac/NOTICE),
[LGPL text](../third_party/libfreeaptx/COPYING),
[LDAC license](../third_party/ldacBT/libldac/LICENSE), and
[LDAC notice](../third_party/ldacBT/libldac/NOTICE).
Sony provides a [licensing inquiry route](https://www.sony.co.jp/en/Products/LDAC/aosp/).
Bluetooth SIG describes [qualification before sale or distribution](https://www.bluetooth.com/develop-with-bluetooth/qualify/).
Applicability and costs for this standalone driver remain open questions.

Earlier versions of this document made broad AAC/aptX patent-expiry assumptions.
Those assumptions have been withdrawn. One expired patent, another product's
implementation, or open-source code availability does not establish clearance in
every target country. Do not use those earlier notes as permission to ship, or
infer a competitor's licensing agreements from its binaries or notices.

## What packages contain

Each codec build generates full license/notice files, `sources-<codec>.zip` and
`sources-<codec>.json` alongside its DLLs. The source archive contains the actual
upstream working files, nested submodules, PN adapter, headers and build scripts.
It has an internal file-hash manifest and rebuild instructions. Git metadata and
private repository access are not needed by recipients.

The outer manifest binds the binary hashes, notice files and source archive.
Setup and portable packaging reject missing or stale redistribution files. Each
installer's codec selection also selects its notices/source; Windows-mode packages
exclude all external codec binaries and archives. See [third-party notices](../THIRD-PARTY-NOTICES.md).

```powershell
git submodule update --init --recursive
.\codec-host\plugins\aac\build.ps1
.\codec-host\plugins\aptx\build.ps1
.\codec-host\plugins\ldac\build.ps1
.\build\test-distribution-compliance.ps1 -Setup .\out\Release\dist\pna2dp-setup.exe
.\build\test-distribution-compliance.ps1 -Folder .\out\Release\dist\portable
```

Scripts in a distributed source archive support `-SkipSourceBundle` to rebuild
the DLLs without the original Git repositories. It does not authorize packaging
without source: packaging still requires a matching bundle. Changing or publisher-
signing a DLL invalidates its recorded hash. From the same frozen release source,
regenerate the bundle after signing and before packaging; do not pair a signed old
binary with newer source files.

These tests verify artifact presence and consistency. They do not assess patents,
grant certification, or certify compliance with every license term. Public release
also needs a source-provenance review, an accurate supported-codec list and hardware
testing. Royalty quotations and any needed agreements are user tasks.

## Branding and future codecs

Use aptX and LDAC names only as factual codec identifiers pending the branding
review; do not add certification logos or imply endorsement. Open-source encoder
availability does not establish trademark permission.

aptX Adaptive/Lossless, LC3plus and LHDC are not implemented here. Do not advertise
them or promise Dolby/DTS home-theater bitstreams through A2DP. Only the Sony SBC
path currently has physical receiver validation here.
