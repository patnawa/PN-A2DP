# Testing plan and current verification status

## Verified in this tree

| Item | How | Result |
|---|---|---|
| SBC encoder/decoder | `out\Release\sbc_test.exe` (frame length formula, CRC, bitpool live change, encode-decode SNR on multi-tone signals for 7 configurations) | passes: 47 dB SNR at joint-stereo bitpool 53, 66 dB at bitpool 128/250, 4- and 8-subband paths |
| Kernel driver compiles | `build\build.ps1 -Target driver` with MSVC 14.44 + WDK 10.0.26100 NuGet, `/W4`, CFG | builds `pna2dp.sys` |
| Codec host, control tool | `build\build.ps1 -Target host`, `-Target cli` | builds |

## Not yet verified (requires a machine in test-signing mode and a kernel debugger)

The analysis PC runs Secure Boot with the commercial driver installed; loading a
test-signed driver there was out of scope. The following sequence brings the driver up
on a development machine:

1. `bcdedit /set testsigning on`, reboot; `.\build\build.ps1 -Sign`; install the test
   certificate; `.\build\install.ps1`.
2. Attach WinDbg (network or local kernel debugging) and set `DebugLevel=4`.
3. Pair an SBC-only sink (any speaker) and verify, in order:
   * `pna2dpctl list` shows the device; `pna2dpctl status` shows `idle`.
   * Device Manager: "PN-A2DP Bluetooth Stereo (A2DP) Device" without a yellow mark;
     Sound settings: endpoint "Speakers (<headset>)" present.
   * `pna2dpctl caps` lists the SEPs after the opportunistic connect (ACL comes up when
     the headset is switched on; DISCOVER runs 1.5 s later).
   * Play audio: `watch` shows configured -> open -> streaming, `stats` shows packets
     and no drops; audio is heard; pause: SUSPEND after 3 s; resume.
   * Volume slider: topology volume property; with a sink that supports absolute volume
     Windows' AVRCP service forwards it (no attenuation in the PCM path by default).
   * Change `sbc.bitpool`: bitrate changes in band without a reconnect.
   * Headset-initiated connection (switch the headset off and on while audio plays).
   * SCO: make a call with a headset that also supports HFP; A2DP suspends and resumes.
   * Surprise removal: switch the headset off mid-stream; driver reports media down and
     reconnects when it returns.
4. Start `pna2dp-codechost.exe` with the LDAC/aptX/AAC plugins and repeat with
   `pna2dpctl set codecs=ldac,aptxhd,aac,aptx,sbc`; verify the status shows `codec host`
   and the sink reports the codec (headset app or HCI sniff).
5. Kill the host while streaming: the driver must fall back to SBC within a second.
6. Exclusive-mode WASAPI at 96 kHz/24-bit with LDAC and `allrates=1`.
7. Driver Verifier (standard settings + DDI compliance) for 24 h of streaming.

## Automated tests to add

* Unit tests for `a2dp_codec.cpp` (capability parsing/serialisation round trips with
  real IEs captured from devices) and for the AVDTP message builder/parser, compiled in
  user mode like the SBC test.
* An AVDTP loopback: a user-mode AVDTP sink simulator attached through a virtual
  Bluetooth controller (BlueZ `btvirt` under WSL is not reachable from Windows; the
  practical path is a second PC with the Windows sink role).
* HLK: Audio (WaveRT), Bluetooth profile driver and Device.Streaming suites before any
  attestation-signed release.
