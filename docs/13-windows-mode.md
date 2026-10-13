# Windows mode

Control panel 0.1.0.21 supports the Microsoft Bluetooth A2DP driver already in
Windows. No PN-A2DP kernel driver, codec host, test certificate, Test Mode or
Microsoft developer registration is needed for this mode.

Run `out/Release/dist-windows/pna2dp-setup.exe` to install the Windows-mode
application, or run `portable/pna2dp-panel.exe` directly. The installer needs UAC
for Program Files and shortcuts; the panel's normal audio controls do not.
The unsigned application may still show a Windows publisher/reputation prompt.
This is separate from kernel driver signing.

The installer preserves existing device driver assignments. If upgrading from
the PN-A2DP driver, select the device, open **Driver**, and choose **Use Windows
mode**. This changes only that device and preserves its PN-A2DP codec preferences.
If Windows requires a restart, save your work and restart manually. The panel
reports a pending change; the selected driver in Device Manager can already say
Microsoft while the old driver still serves an open audio stream.

Devices on the Microsoft driver show **Quality**, **Audio** and **Driver** tabs
in panel 0.1.0.27; application 0.1.0.34 orders them **Audio**, **Quality**, **Driver**
and opens on Audio. Quality offers read-only audio checks and diagnostic export;
Bluetooth codec/packet telemetry remains unavailable in this mode.
Devices on PN-A2DP retain the existing codec and transport tabs.
Other third-party drivers are explicitly labelled as such; they can use the
standard Windows audio controls without being labelled as Microsoft drivers.

## Available controls

Panel **0.1.0.24** removes the Spatial tab. Existing Windows spatial sound choices
are unchanged; use Windows Sound settings to manage them. The diagnostic CLI
still supports read-only spatial queries.

The Driver tab shows labelled lights for the actual running Test Mode, Secure
Boot and Memory Integrity states. Green means a requirement for the development
PN driver is met, red means it is not, and amber means unknown or a pending change.
It does not indicate that turning security features off makes Windows safer.
For this development build, activation requires Test Mode **ON**, Secure Boot
**OFF**, and Memory Integrity **OFF**. The HVCI-off check is the validated PN
development configuration, not a claim that all test-signed drivers require it.
Microsoft-signed release packages do not require these development restrictions.

Open **Requirements...** for all blockers and recovery steps. **Windows Security**
opens Core isolation without changing it. Close **Windows / Test Mode** before
switching drivers. A shared operation guard prevents overlapping boot and
driver operations. Unknown status or unverified package files block activation;
the elevated helper verifies the package, actual running state and next-boot
Test Mode setting again before it binds the driver. Pending enables do not count
until Windows has restarted. The application never turns security features off
or restarts the machine automatically.

Panel 0.1.0.23 adds **Driver > Windows / Test Mode...**. Windows asks for
administrator permission to inspect the boot entry. The dialog separates the
current session from the setting saved for the next start of this Windows
installation. **Enable Test Mode** and **Normal Windows (Test Mode OFF)** require
confirmation and take effect after a manual restart; the program never restarts
Windows automatically.

To return to normal Windows operation, first select **Use Windows mode** for
devices that use the development PN driver. Disabling Test Mode is blocked while
a paired PN device still uses a driver whose Microsoft catalog signature cannot
be verified, including disconnected devices. Microsoft-signed PN packages can
continue in normal mode. Other test-signed drivers on the machine may also need
Test Mode. Secure Boot, BitLocker, Memory Integrity and certificates are not
changed by this dialog. A Windows restriction is shown rather than bypassed.

The next-start value is read as a typed Boolean through the BCD WMI provider,
not by parsing localized BCDEdit text. Missing/unreadable values remain explicit.
Writes affect only `{current}`'s `testsigning` option and are read back before
success is reported. `boot_mode_test` exercises both transitions, pending-change
cancellation, policy/dependency failures and failed readback with a fake backend;
`boot_mode_test --probe` adds an elevated, read-only check of the real BCD store.

- Selected device's master output volume and mute, applied immediately. Changes
  made in Windows are reflected on the next one-second refresh.
- Active output name, default multimedia output status, Windows mix format,
  adapter identity, and driver information.
- Links to Windows Sound and Bluetooth settings.
- Read-only diagnostics: `pna2dpctl windows list`, `windows status -d 0`, and
  `windows spatial -d 0`. These indices cover all paired A2DP devices, unlike
  the original PN-driver-only command indices.

Codec selection, SBC bitpool, transmitted bitrate, receiver codec capabilities,
and per-packet transport health are not supplied by this backend. The program
does not pretend that a 48 kHz / 32-bit float Windows mix is the Bluetooth wire
format, or that an adapter version establishes usable audio bandwidth. Existing
encoder plugins cannot be inserted into the Windows Bluetooth encoder.

## Implementation and validation

`pnaudio` resolves an active render endpoint by its topology adapter's exact PnP
ancestry. It refuses missing or ambiguous matches, never falls back to the default
speaker, and rechecks endpoint identity before writes. Invalid volume values and
stale endpoint IDs are rejected. Property-query failures remain unavailable,
rather than being displayed as zero volume or a guessed codec. `pnspatial` shares
the same resolver. The controls run in user mode through Core Audio.

Build the application-only installer and portable package:

```powershell
.\build\build.ps1 -Target windows
.\build\build.ps1 -Target tests
.\out\Release\windows_audio_test.exe
.\build\test-windows-mode.ps1
```

An explicit hardware test can target a Bluetooth address with `--device <hex>`.
`--require-inbox` rejects a pending driver restart. `--exercise` briefly mutes and
lowers that output, checks readback, and restores the original volume/mute. No
live write is performed by the default test. A normal-mode boot and subjective
listening must be confirmed separately; successful Core Audio calls do not prove
that a pending driver replacement has taken effect or that sound is audible.

References: [Core Audio volume](https://learn.microsoft.com/en-us/windows/win32/api/endpointvolume/nn-endpointvolume-iaudioendpointvolume),
[running code integrity flags](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation),
[Test Mode and HVCI](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option),
[Windows Bluetooth codecs](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/bluetooth-classic-audio),
[Windows mix format](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getmixformat).
