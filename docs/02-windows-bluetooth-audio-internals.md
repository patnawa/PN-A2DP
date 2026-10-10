# Windows Bluetooth classic audio internals (Windows 11 build 26100.9444)

This is the reference the PN-A2DP driver is built against. Everything here was read
from inbox INFs, driver import and export tables, the registry and Microsoft
documentation.

## 1. Enumeration: how an A2DP driver gets loaded

```
BTHUSB.sys  (bus miniport)      USB\VID_0489&PID_E10A  Qualcomm FastConnect 7800
  BTHPORT.sys                   HCI, L2CAP, SDP, SCO; interface GUID_BTHPORT_DEVICE_INTERFACE
    BthEnum.sys                 bus driver, creates one PDO per remote SDP service
      BTHENUM\DEV_<bdaddr>                     "Bluetooth Device" (bth.inf)
      BTHENUM\{0000110b-...}_LOCALMFG&001d     remote AudioSink   -> A2DP source driver (MEDIA)
      BTHENUM\{0000110a-...}                   remote AudioSource -> A2DP sink driver   (MEDIA)
      BTHENUM\{0000110c-...} / {0000110e-...}  AVRCP TG/CT        -> Microsoft.Bluetooth.AvrcpTransport.sys
      BTHENUM\{0000111e-...}                   HFP                -> bthhfenum.sys
```

* Hardware ID: `BTHENUM\{uuid}_LOCALMFG&xxxx` where xxxx is the local radio
  manufacturer (001d = Qualcomm). Compatible IDs: `BTHENUM\{uuid}` and
  `BTHENUM\{uuid}_GENERIC`.
* `Bluetooth_UniqueID` under `Device Parameters` is `{uuid}#<bdaddr>_C00000000`.
* The container ID of the PDO equals the container ID of the paired device. That is how
  the audio endpoint, the Bluetooth settings page and the AVRCP service correlate a
  volume endpoint with a Bluetooth device.
* The inbox source INF `microsoft_bluetooth_a2dp_src.inf` has `PreConfigureDriver=*`.
  Both the inbox and an attestation-signed third-party driver count as Microsoft signed,
  so the inbox driver tends to win on date and version. Product installers therefore use
  `DiInstallDevice` with `DIIRFLAG_FORCE_INF`.

## 2. The profile driver DDI (kernel)

Query `GUID_BTHDDI_PROFILE_DRIVER_INTERFACE` with `WdfFdoQueryForInterface` to get
`BTH_PROFILE_DRIVER_INTERFACE` (`BthAllocateBrb`, `BthFreeBrb`, `BthReuseBrb`,
`BthInitializeBrb`). All transport operations are BRBs sent as
`IOCTL_INTERNAL_BTH_SUBMIT_BRB` (internal IOCTL, `OtherArg1 = BRB`) to the PDO's
default I/O target:

| BRB | Purpose for A2DP |
|---|---|
| `BRB_HCI_GET_LOCAL_BD_ADDR` | local address (SDP record, logging) |
| `BRB_REGISTER_PSM` (`_BRB_PSM`) | register PSM 0x0019 (AVDTP) so sinks can connect to us |
| `BRB_L2CA_REGISTER_SERVER` | incoming connection indications for AVDTP |
| `BRB_L2CA_OPEN_ENHANCED_CHANNEL` | open the signalling channel, then the media channel, to PSM 0x0019 |
| `BRB_L2CA_OPEN_ENHANCED_CHANNEL_RESPONSE` | accept remote-initiated channels |
| `BRB_L2CA_ACL_TRANSFER` | read (continuous readers) and write L2CAP SDUs |
| `BRB_L2CA_CLOSE_CHANNEL` | close channels |

Other IOCTLs on the PDO target:

* `IOCTL_INTERNAL_BTHENUM_GET_DEVINFO` returns `BTH_DEVICE_INFO` (remote address,
  name, class of device).
* `IOCTL_BTH_SDP_CONNECT`, `IOCTL_BTH_SDP_SERVICE_ATTRIBUTE_SEARCH`,
  `IOCTL_BTH_SDP_DISCONNECT`: read the remote AudioSink record (AVDTP version,
  SupportedFeatures: headphone, speaker, recorder, amplifier bits).
* `IOCTL_BTH_SDP_SUBMIT_RECORD`: publish our own AudioSource (0x110A) record when the
  inbox stack has not already done so.
* `IOCTL_BTH_GET_HOST_SUPPORTED_FEATURES`: ERTM availability.

Connection indications arrive at DISPATCH_LEVEL through the callback given in
`BRB_L2CA_REGISTER_SERVER` or `OPEN_CHANNEL` (`IndicationRemoteConnect`,
`IndicationRemoteDisconnect`, `IndicationRemoteConfigRequest`).

AVDTP signalling and media both use L2CAP Basic mode in practice. The media channel
MTU should be negotiated as large as the controller allows (typically 672 to 1024
bytes). A bigger MTU means fewer and larger ACL packets per frame, which matters for
LDAC at 990 kbps.

## 3. PnP custom events on the radio

`IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange, radioFileObject)`
delivers `TARGET_DEVICE_CUSTOM_NOTIFICATION` with these GUIDs (bthdef.h):

* `GUID_BLUETOOTH_HCI_EVENT` with `BTH_HCI_EVENT_INFO { bthAddress, connectionType (1 ACL, 2 SCO, 3 LE), connected }`.
  This is the SCO detector. When a SCO link to our device comes up, HFP owns the speaker.
* `GUID_BLUETOOTH_L2CAP_EVENT` with `BTH_L2CAP_EVENT_INFO { bthAddress, psm, connected, initiated }`.
* `GUID_BLUETOOTH_RADIO_IN_RANGE` and `GUID_BLUETOOTH_RADIO_OUT_OF_RANGE`.

## 4. The inbox A2DP driver (BthA2dp.sys 10.0.26100.9278)

* AVStream (`ks.sys`) filter driver, KMDF, both source and sink roles (two KS filter
  factories "SRC" and "SNK"; the sink endpoint is what the Store "Bluetooth Audio
  Receiver" app and `Windows.Media.Audio.AudioPlaybackConnection` use).
* Codecs: SBC, AAC (`BluetoothAacEnable`, `AacUse48kHz`, `AACDropLargePackets`,
  `AacL2capSize`, `AacMtu`), aptX classic, and the sideband offload path for aptX
  Adaptive (`A2DP_SIDEBAND_INTERFACE`, `A2dpOffloadDisabled`, `DisableOffload`,
  `HCI_VS_MSFT_Avdtp_*`). With offload the SoC audio driver encodes and the controller
  sends media packets (`sidebandaudio.h`, `a2dpsidebandaudio.h`); the profile driver
  only does AVDTP signalling.
* Registry: `HKLM\SYSTEM\CurrentControlSet\Services\BthA2dp\Parameters`
  (`BluetoothAacEnable`, `ForceLogsInMiniDump`, `WppRecorder_UseTimeStamp`) and
  `HKLM\SYSTEM\CurrentControlSet\Control\Bluetooth\Audio\A2dp\{Source,Sink}\Enabled`.
  Other strings: `SuspendDelay`, `DelayReportDelay`, `SendDiscoverASAP`,
  `CongestionTestMode`, `BthBVRADelay`, `BthVCDelay`, `BthVCTeardownDelay`.
* Uses `btampm.sys` ("Bluetooth Audio Multi-Profile Manager"; exports
  `BtaMpmRegister`, `BtaMpmRegisterPnp`, `BtaMpmUpdateConnectionStatus`,
  `BtaMpmUpdatePlayStatus`, `BtaMpmUpdateSuspendStatus`, `BtaMpmSuspendRequest`,
  `BtaMpmConnectionRequest`, `BtaMpmGetStatus`,
  `BtaMpmGetRemoteDeviceProfileVersionAndAttribute`) to arbitrate between HFP and
  A2DP. This is the kernel side of the Windows 11 unified endpoint. The DDI is
  undocumented; the AVRCP transport driver also uses it.
* Codec priority (Windows 11): aptX Adaptive (Qualcomm radios only, 24H2) > AAC > aptX
  > SBC. Windows 10: aptX > SBC. There is no public way to add codecs to the inbox
  driver, and no native LDAC or aptX HD as of October 2026.

## 5. AVRCP and absolute volume

* `Microsoft.Bluetooth.AvrcpTransport.sys` binds the 0x110C and 0x110E PDOs and
  provides an AVCTP transport to the user-mode `BthAvctpSvc` service
  (`BthAvctpSvc.dll`). That service implements the AVRCP target (media keys to the
  System Media Transport Controls) and the absolute-volume controller, bridging to the
  audio endpoint volume (`audiolibcontrolclient.cpp`, `avrcpmediabridge.cpp`). It reads
  `Control\Bluetooth\Audio\AVRCP\CT\DisableAbsoluteVolume` and correlates endpoints by
  container ID (`BluetoothAudioTransportSelector`).
* A third-party A2DP driver therefore gets absolute volume for free as long as its
  endpoint lives under the Bluetooth device container (it does: the FDO sits on the
  BthEnum PDO) and the topology exposes a standard `KSNODETYPE_VOLUME`. When the sink
  sends `EVENT_VOLUME_CHANGED` the service sets the endpoint volume, which reaches the
  topology volume property handler; when the user moves the Windows slider the service
  sends `SetAbsoluteVolume`. The driver should treat the volume node as a 0 dB software
  gain by default (the sink applies the real gain) and apply digital attenuation only
  when absolute volume is disabled or unsupported by the sink.

## 6. Windows audio stack facts that shape the design

* PortCls WaveRT with event-driven packet mode (`IMiniportWaveRTOutputStream`,
  `KSPROPERTY_RTAUDIO_*`, `PKEY_AudioEndpoint_Supports_EventDriven_Mode`) is what both
  the inbox and the Alternative driver expose. The audio engine writes PCM into a cyclic
  buffer and the miniport reports position; the driver's own thread consumes it.
* The engine's shared-mode format is fixed per endpoint ("Default format" in the Sound
  control panel). Exclusive-mode apps can pick any advertised format. Windows 11 unified
  endpoints pick the first advertised format, so the format table must be ordered by
  preference and trimmed to what the negotiated codec supports.
* Latency reporting: `IMiniportWaveRTStream::GetHWLatency` (`ChipsetDelay`,
  `CodecDelay`, `FifoSize`) reaches applications through `IAudioClient::GetStreamLatency`
  and `KSPROPERTY_RTAUDIO_PRESENTATION_POSITION`. This is the channel to expose
  Bluetooth encode, air and sink delay (AVDTP delay reports).
* APOs: the `FX\0` registration in the INF (`mssysfx`, "Audio Home Theater Effects")
  keeps bass boost, virtual surround and loudness available. Third-party INFs must use
  `FX\\n`, not `MSFX\\n`.

## 7. Signing reality (2026)

* 64-bit Windows 10 and 11 load only kernel drivers signed by Microsoft (attestation
  signing through Partner Center, which requires an EV code-signing certificate and a
  Hardware Dev Center account) or HLK/WHQL-signed drivers. Self-signed drivers need test
  signing mode (`bcdedit /set testsigning on`).
* Secure Boot and HVCI: attestation-signed drivers load; test-signed drivers need test
  mode.
* Open-source projects commonly ship a test-signed build for developers and an
  attestation-signed release signed by a maintainer who owns an EV certificate.
