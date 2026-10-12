# Spatial sound with PN-A2DP

PN-A2DP transports the **stereo output of Windows' headphone renderer**. It does not
encode Dolby or DTS bitstreams and does not add a Dolby/DTS decoder to a Bluetooth
receiver. The existing 0.1.0.14 driver already exposes a compatible PCM endpoint;
`pna2dpctl spatial [-d N]` provides read-only discovery and status reporting.
The Spatial tab introduced in panel 0.1.0.15 was removed in 0.1.0.24. This GUI
change does not alter Windows spatial settings or require replacing the driver.

| Format | PN-A2DP route | Requirements |
|---|---|---|
| Windows Sonic for Headphones | PC renders binaural stereo, then the selected A2DP encoder compresses it | Windows reports support; enable it in Windows sound settings |
| Dolby Atmos for Headphones | Same stereo route, with Dolby's Windows renderer | Compatible Windows endpoint and Dolby Access activation/license |
| DTS Headphone:X | Same stereo route, with DTS' Windows renderer | Compatible Windows endpoint and DTS Sound Unbound activation/license |
| Dolby Atmos for home theater | Unsupported over PN-A2DP | Compatible HDMI/eARC endpoint, receiver, content and Windows/playback support |
| DTS:X for home theater | Unsupported over PN-A2DP | Compatible HDMI/eARC endpoint, receiver, content and Windows/playback support |

Headphone spatial audio can work through ordinary stereo Bluetooth headphones. The
headphones do not need to advertise Dolby or DTS in AVDTP. A receiver advertising
SBC/AAC/aptX/LDAC does **not** establish that it accepts Atmos/DTS:X bitstreams.
Home-theater decoding and headphone virtualization are different output paths.

Open **Windows Settings > System > Sound**, choose the same named
output, then select **Spatial sound**. Windows and the vendor apps own the options
and activation. The diagnostic CLI reports selected and active formats separately: a
selected provider can be inactive if Windows temporarily uses another output mode.
An installed app is not proof of an unlocked license. A supported format is not
proof that the current music/game is delivering spatial content.

Use headphone modes with headphones. The tested Sony CMT-SBT40D is correctly
identified as speakers; headphone binaural processing can sound unnatural through
speakers. PN-A2DP does not change the form factor to force options into Windows.
Regular stereo music does not gain original height/object metadata by enabling a
spatial renderer. Avoid stacking another headphone surround processor on the same
audio. Exclusive/raw playback paths can bypass Windows spatial processing.

## Bandwidth and quality

Windows mixes and spatializes locally before Bluetooth compression, so these
headphone modes do not add transmitted channels or require a higher A2DP bitrate.
The tested Sony negotiates SBC at 48 kHz, two channels, 16-bit encoder input and
357 kbps. LDAC can use 990 kbps at 48/96 kHz, or 909 kbps at 44.1/88.2 kHz, with a
receiver supporting LDAC and sufficient radio throughput. Radio overhead,
interference and retransmissions mean the advertised Bluetooth PHY speed is not
the application's sustained audio bandwidth.

For scale, uncompressed PCM at 48 kHz requires 1.536 Mbps for stereo/16-bit or
9.216 Mbps for eight channels/24-bit (`rate * channels * bits`). These are PCM
payload calculations, not claimed Atmos/DTS:X bitrates. Some compressed multichannel
formats have lower bitrates, but fitting a bitrate alone does not add the required
bitstream format, object metadata or receiver/transport support. PN-A2DP's codec
pipeline accepts stereo PCM and sends its negotiated A2DP codec only.

SBC/AAC/aptX/LDAC remain lossy transports here. Spatial processing cannot recover
lost codec information or remove Bluetooth latency. Automatic codec selection and
ABR continue to respect the receiver's advertised capabilities and actual congestion.
Higher bitrates are not a guarantee of a better result on an unreliable link.

## Detection implementation

`tools/common/pnspatial.cpp` uses documented Windows APIs:

- Enumerate active render endpoints through MMDevice. Follow `IDeviceTopology`'s
  connector to the audio adapter and match its PnP ancestry exactly to the selected
  Bluetooth service instance. Never match a friendly name or use the default output.
- Enumerate `DEVINTERFACE_AUDIO_RENDER` with Configuration Manager and match the
  same ancestry. Reject ambiguous or missing endpoints instead of guessing.
- Pass the actual render **device-interface path** to
  `SpatialAudioDeviceConfiguration::GetForDeviceId`. Passing the MMDevice endpoint
  ID instead silently reports all formats unsupported on the tested Windows build.
- Query `IsSpatialAudioFormatSupported` with the official `SpatialAudioFormatSubtype`
  IDs. Query `ActiveSpatialAudioFormat` and `DefaultSpatialAudioFormat` independently.
- Detect vendor packages for the current user with `GetPackagesByPackageFamily`;
  do not infer license state. Preserve unknown/query-failure states.

No registry patch, custom Dolby/DTS APO, codec licensing bypass, fake multichannel
format or third-party provider registration is involved. Windows only permits a
format's owning app to set it through `SetDefaultSpatialAudioFormatAsync`; PN-A2DP
opens Windows settings so users select it through the supported system UI.

## Verification on this machine

Windows 11 build 26300, Qualcomm FastConnect 7800, PN-A2DP driver 0.1.0.14 and Sony
CMT-SBT40D: the Windows dropdown and live API both expose Sonic, Dolby Atmos for
Headphones and DTS Headphone:X. Neither home-theater format is supported on this
endpoint. Dolby Access and DTS Sound Unbound are installed.

Dolby, DTS and Windows Sonic were each selected in Windows, confirmed as the active renderer, and
tested with a five-second 48 kHz float spatial object stream (front left/right,
silence). All three render loops completed successfully through the selected PN-A2DP
endpoint. This verifies API/render-pipeline operation, not subjective headphone
localization or other Bluetooth receivers/codecs. Spatial sound is restored to Off
after testing the speaker output.

`spatial_test` checks missing-device behavior, no default-output fallback, exact live
endpoint/interface discovery, STA/MTA calls, and the separation between unknown
status and unsupported transport. `--require-live` requires a connected output;
`--render-silent` additionally tests the currently configured spatial renderer.
The tool never changes settings. `build/verify.ps1` includes the discovery regression.

## References

- [Microsoft Spatial Sound](https://learn.microsoft.com/en-us/windows/win32/coreaudio/spatial-sound)
- [SpatialAudioDeviceConfiguration](https://learn.microsoft.com/en-us/uwp/api/windows.media.audio.spatialaudiodeviceconfiguration)
- [SpatialAudioFormatSubtype](https://learn.microsoft.com/en-us/uwp/api/windows.media.audio.spatialaudioformatsubtype)
- [Rendering spatial audio objects](https://learn.microsoft.com/en-us/windows/win32/coreaudio/render-spatial-sound-using-spatial-audio-objects)
- [Dolby Atmos on headphones](https://www.dolby.com/en-gb/experience/home-entertainment/articles/how-to-experience-dolby-atmos-on-headphones/)
- [DTS Sound Unbound FAQ](https://consumer.dts.com/dts-sound-unbound-faq/)

Dolby, Dolby Atmos, DTS and DTS:X identify third-party technologies. PN-A2DP is not
a Dolby/DTS-certified product and does not bundle their proprietary renderers.
