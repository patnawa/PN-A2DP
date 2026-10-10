# Deep analysis: Bluetooth Goodies "Alternative A2DP Driver" (v1.8.3.1)

Analysis date: 2026-10-10. Sources: the public site (bluetoothgoodies.com/a2dp), the
installed product on the analysis PC (`C:\Program Files\Luculent Systems\AltA2DP`), the
driver package in the Driver Store (`alta2dp.inf_amd64_f81fce6c1599eba4`), PE import and
string tables of every binary, and the live registry state while a Sony CMT-SBT40D
speaker was paired. No decompilation was performed; everything below is observable from
headers, INF, registry and strings.

## 1. What the product is

| Item | Value |
|---|---|
| Vendor | Luculent Systems, LLC (brand "Bluetooth Goodies") |
| Product | Alternative A2DP Driver, v1.8.3 (installer 1.8.3.1, built 2026-06-18) |
| Role | A2DP **source only** (PC sends audio to headphones). No sink role. |
| Supported OS | Windows 10 1709+ x64, Windows 11 x64 and arm64 |
| Price | USD 9.99 regular, 5.99 during the 7-day trial; AAC edition costs more |
| License | Perpetual, locked to the motherboard, one PC, not transferable |
| Distribution | MSI (WiX 3.14), EV code-signed by SSL.com, driver attestation-signed |

The product replaces the Windows inbox A2DP driver (`BthA2dp.sys`) per audio device and
adds codec choice plus codec parameter tuning. Its selling points are SBC bitpool and
Dual Channel control, every sampling frequency the headset reports, AAC on Windows 10,
and aptX HD, aptX Low Latency and LDAC, which Windows never offers natively.

## 2. Package contents

```
Driver\AltA2DP.sys   823 KB  kernel driver (KMDF 1.27 + PortCls WaveRT)
Driver\AltA2DP.inf          binds BTHENUM\{0000110b-...} (remote AudioSink service)
Driver\AltA2DP.cat          attestation catalog
AltA2dpSVC.exe        75 KB  Win32 service "AltA2dpSVC" (auto start)
AltA2dpConfig.exe    3.1 MB  WinUI 3 / XAML configuration app
AltA2dpDriverInstaller.exe  elevated helper: SetupCopyOEMInf + DiInstallDevice
Docs\FAQ.html, eula.html, 3rdparty.html
```

Third-party notices list only WiX, nlohmann/json, AOSP (`system/bt`, i.e. the Android
SBC encoder and `libldac`) and Fraunhofer FDK AAC. No aptX library is credited, so the
aptX, aptX HD and aptX LL encoders are in-house or from an uncredited source.

## 3. How the driver attaches to Windows (from the INF)

```
%Desc% = AltA2DP10, BTHENUM\{0000110b-0000-1000-8000-00805f9b34fb}   ; Win10 1709+
%Desc% = AltA2DP11, BTHENUM\{0000110b-0000-1000-8000-00805f9b34fb}   ; Win11
Include = ks.inf, wdmaudio.inf
Needs   = KS.Registration, WDMAUDIO.Registration, mssysfx.CopyFilesAndRegister(CapX)
AddInterface KSCATEGORY_AUDIO / KSCATEGORY_TOPOLOGY  "Topology"
AddInterface KSCATEGORY_AUDIO / KSCATEGORY_RENDER    "Wave"
```

* Class is `MEDIA`, the same as the inbox driver, so Device Manager shows it under
  "Sound, video and game controllers".
* The hardware ID is the generic compatible ID of the BthEnum PDO that represents the
  remote device's AudioSink (0x110B) SDP service. Windows prefers the inbox driver
  (`microsoft_bluetooth_a2dp_src.inf`, same ID, `PreConfigureDriver=*`), so the product
  installer forces the driver with `DiInstallDevice`, and Windows Update can later
  revert it (the FAQ warns about this).
* The topology filter registers the Microsoft "Audio Home Theater Effects" APO
  (`mssysfx`) exactly like the inbox INF, so enhancements and spatial sound keep working.
* `PnpLockDown=1`, service `AltA2DP`, demand start, binary runs from the Driver Store.

## 4. Driver architecture (from imports and strings)

Imports: `portcls.sys` (`PcInitializeAdapterDriver`, `PcAddAdapterDevice`, `PcNewPort`,
`PcRegisterSubdevice`, `PcRegisterPhysicalConnection`, `PcNewServiceGroup`,
`PcRegisterAdapterPowerManagement`, `PcDispatchIrp`), `WDFLDR.SYS` (KMDF 1.27 bound as
a miniport driver), `WppRecorder.sys`, `ntoskrnl.exe`.

Conclusions:

1. It is a classic PortCls adapter driver with a WaveRT render miniport and a Topology
   miniport ("Alternative A2DP Wave", "Alternative A2DP Topology"). This is the
   simplest way to appear as a normal Windows audio endpoint.
2. It is not AVStream based, unlike the inbox `BthA2dp.sys` (which imports `ks.sys`).
3. All Bluetooth I/O goes through KMDF I/O targets to BthPort (BRBs through
   `IOCTL_INTERNAL_BTH_SUBMIT_BRB`). The GUID table contains
   `GUID_BTHPORT_DEVICE_INTERFACE`, `GUID_BLUETOOTH_HCI_EVENT`,
   `GUID_BLUETOOTH_L2CAP_EVENT` and `GUID_BLUETOOTH_RADIO_IN_RANGE/OUT_OF_RANGE`, so it
   opens the radio's device interface and registers for PnP custom notifications. That
   is how it detects "SCO active" (HCI event with `connectionType == SCO`).
4. `KeSaveExtendedProcessorState` / `KeRestoreExtendedProcessorState` are imported:
   all codecs run in kernel mode (LDAC and AAC use floating point and SIMD).
5. It creates its own system threads and high-resolution timers: a dedicated streaming
   thread pulls PCM from the WaveRT cyclic buffer, encodes, packetizes and submits
   L2CAP writes.
6. `IoRegisterDeviceInterface` plus a hand-built security descriptor: a private,
   ACL-restricted control interface used by the service and the config app.
7. `ZwQuerySystemInformation`, `PsGetVersion`, `RtlGetVersion`: version gating and
   license enforcement.
8. It reads `HKLM\SYSTEM\CurrentControlSet\Control\Bluetooth\Audio\AVRCP\CT`
   (`DisableAbsoluteVolume`) exactly as the inbox driver does.
9. It does not import `btampm.sys` (Microsoft's Bluetooth Audio Multi-Profile Manager).
   The inbox driver uses BtaMpm to coordinate HFP and A2DP for the Windows 11 unified
   endpoint. This explains every "Unified Audio Endpoint" and "microphone switches audio
   to mono" caveat in the FAQ: a third-party driver cannot join the unified endpoint, so
   the product can only observe SCO and suspend.

## 5. Registry schema (observed live)

```
HKLM\SYSTEM\CurrentControlSet\Services\AltA2DP\Parameters
    LogPages=16, VerboseOn=1, DriverVersion=0x01080301
    \Devices\Capability\<bdaddr>   what the headset advertised (AVDTP GET_CAPABILITIES)
    \Devices\Current\<bdaddr>      what is configured now plus live status
    \Devices\Next\<bdaddr>         what the user wants on the next (re)connect
HKLM\SYSTEM\CurrentControlSet\Services\AltA2dpSVC\Parameters   service settings
HKLM\SOFTWARE\Luculent Systems\Alternative A2DP Driver\License  TrialLicense blob
```

Example for the Sony CMT-SBT40D (SBC-only sink):

| Key | Values |
|---|---|
| Capability | `Codec=1` (bit0 = SBC), `SbcChannelMode=15`, `SbcSamplingFrequency=15`, `SbcAllocationMethod=3`, `SbcSubbands=3`, `SbcBlockLength=15`, `SbcMinimumBitpool=2`, `SbcMaximumBitpool=53` (raw A2DP SBC capability masks) |
| Current | `Codec=1`, `SbcChannelMode=4` (Dual Channel), `SbcSamplingFrequency=1` (48 kHz), `SbcAllocationMethod=1` (Loudness), `SbcSubbands=1` (8), `SbcBlockLength=1` (16), `SbcMaximumBitpool=53`, `AbrEnable=1`, `Opened=1`, `Bitrate=672000`, `ScoActive=0`, `Delay=0`, `TxTimeStats`, `TxTimeMax=10` |
| Next | same plus `AacChannelMode`, `AacSamplingFrequency`, `LdacEqmid`, `AptxHd*`, `AptxLl*`, `VolumeLevel=0xFFF78AF1` (-8.46 dB in KS 16.16 fixed point) |

Observations:

* The bit masks follow the A2DP specification byte layout directly (SBC: 48 kHz is
  bit 0, Dual Channel is bit 2). 672 kbps is Dual Channel, 48 kHz, 16 blocks, 8
  subbands, bitpool 53, the "SBC XQ" style configuration.
* `Current` is the live status feed for the config app (bitrate, SCO active, delay,
  error, TX time statistics). The app polls it and uses `RegNotifyChangeKeyValue`.
* `Next` is applied only when the stream is reopened. The driver has no live AVDTP
  RECONFIGURE path.
* Per-device volume is persisted by the driver (`VolumeLevel`).
* Other named knobs in the driver strings: `AutoConnectDelay`, `ReconnectDelay`,
  `DiscoverDelayIn/Out`, `SnkDiscoverTO`, `TxLatency`, `LlBufferLevel`, `LlLatency`
  (aptX LL buffering), `AacAfterBurner`, `AacBandwidth`, `AacBitrateMode`,
  `AacPeakBitrate`, `AacMaxSize`, `LdacEqmid`, `LdacSampleFormat`,
  `AptxHdSampleFormat`, `SbcMinimumBitpool`.

## 6. User-mode pieces

* **AltA2dpSVC.exe**: small service (imports `DeviceIoControl`,
  `CM_Get_Device_Interface_List`, `CM_Enable/Disable_DevNode`, registry notification,
  waitable timers). It enumerates `BTHENUM\{0000110E-...}` (AVRCP PDOs), talks to the
  driver through its control interface, and can disable and re-enable device nodes to
  force a reconnect. It also performs the periodic license and trial check that
  silences audio after the trial.
* **AltA2dpConfig.exe**: WinUI 3 app. Features visible in the embedded XAML: codec
  combo (SBC, AAC, aptX, aptX HD, aptX LL, LDAC), SBC bitpool slider 2..53 with an
  "override max bitpool" option, subbands, block size, allocation method, Joint Stereo
  or Dual Channel, automatic bitrate reduction for SBC and LDAC, LDAC EQMID HQ/SQ/MQ,
  16 or 24 bit for LDAC and aptX HD, per-codec sample-rate check boxes with "not
  supported by device" tooltips, status grids (configured codec, bitrate, "SCO active /
  suspended by HFP"), a reconnect button, "safe parameters", high-bitrate warnings, a
  feedback form, and license and trial panels. It also imports `wlanapi`
  (`WlanGetNetworkBssList`): it scans Wi-Fi networks to warn about 2.4 GHz interference.
* **AltA2dpDriverInstaller.exe**: elevated helper using `SetupCopyOEMInf`,
  `DiInstallDevice` and WinRT `BluetoothAdapter` and `Radio` to toggle the radio.

## 7. Documented behaviour and limitations

* Source role only; no PC-as-speaker.
* Inbox Windows stack and boot loader required; no third-party stacks; no USB
  "transmitter" dongles that appear as USB audio.
* LE Audio devices must be re-paired in classic mode.
* Parameter changes apply only on reconnect.
* Sample-rate switching needs an exclusive-mode player; shared mode follows the Windows
  mixer format. On Windows 11 the unified endpoint assumes one format, so only the first
  (highest quality) format is used unless the user disables the others.
* When HFP or SCO is active, A2DP audio is dropped. The driver shows "SCO active".
* aptX Adaptive and Lossless, LC3 and LE Audio, LHDC, SSC: not supported, no plans.
* Known device issues: multipoint headsets that advertise codecs they cannot accept
  (Edifier STAX), aptX Adaptive devices that omit the mandatory aptX fallback (Bose QC
  Ultra, LG TONE-T90Q), BARROT and UGREEN BT 5.4/6.0 dongles that cap bitrate under
  200 kbps, ASUS AI noise cancelling conflicts, antivirus false positives.
* Trial expiry: headphones still connect but produce silence.

## 8. Strengths worth keeping

1. Minimal footprint: one PortCls driver bound to the existing BthEnum PDO; no filter
   drivers, no changes to BthPort or BthEnum; uninstall restores the inbox driver.
2. Reuses the inbox APO registration so Windows "enhancements" keep working.
3. Correct use of documented DDIs (BRB L2CAP, SDP IOCTLs, PnP custom events).
4. The per-device Capability, Current and Next model is simple and robust.
5. Good operational advice in the FAQ (interference, Class 1 adapters, multipoint).

## 9. Weaknesses and what "better" means

| Weakness | Consequence | PN-A2DP answer |
|---|---|---|
| All codecs in kernel mode | FP and SIMD state juggling, LGPL and patent coupling, a codec bug is a BSOD, codecs cannot be swapped | Kernel keeps only transport plus an SBC fallback; a user-mode codec host runs LDAC, aptX, AAC and Opus plugins through inverted-call IOCTLs |
| Changes apply only on reconnect | Annoying tuning loop | Live AVDTP RECONFIGURE (SUSPEND, RECONFIGURE, START) and in-place bitpool changes without any signalling |
| No delay reporting | No A/V sync information for players | AVDTP 1.3 Delay Reporting consumed and exposed through WaveRT `GetHWLatency` and presentation position |
| Only observes SCO | Audio silently drops during calls | Same SCO detection plus explicit AVDTP SUSPEND and START around SCO so the OS mixer does not glitch |
| Registry polling for status | Laggy UI, no events | Event-driven status IOCTL (a pending request completed on change) plus structured ETW |
| Closed source, paid, motherboard-locked | Cannot audit, cannot fix device quirks | Open code, open device quirk table |
| No sink role | PC cannot be a speaker | Design includes the AudioSource (0x110A) PDO binding and a capture pin (phase 2) |
| Windows Update reverts the driver | Audio silently falls back to inbox codecs | Install helper re-applies the driver and notifies the user |
| ABR is a simple "reduce quality on loss" | Oscillation, slow recovery | Closed-loop ABR driven by L2CAP TX queue depth and completion latency, with hysteresis and codec-specific ladders |
| Single format exposed on Win11 | Loses 96 kHz and 24 bit | Expose only the formats the active codec can carry and reconfigure when the engine changes format |
| No FastStream or bidirectional codec | Microphone use forces HFP mono | Optional FastStream back-channel on sinks that support it (phase 2) |

See `03-architecture.md` for the design that implements these answers.
