# PN-A2DP 0.1.0.34: download, Test Mode setup and recovery

Application **0.1.0.34**, custom driver **0.1.0.21**, Windows x64.
This is an **experimental prerelease**, not a Microsoft-signed retail driver.

## Choose your package

| Download | Purpose | Test Mode required? |
|---|---|---|
| `PN-A2DP-0.1.0.34-windows-setup.exe` | Control panel using the existing Windows Bluetooth driver | No |
| `PN-A2DP-0.1.0.34-windows-portable.zip` | Windows-mode application without installation | No |
| `PN-A2DP-0.1.0.34-testmode-setup.exe` | Custom PN driver, codec host, control panel and codec plugins | Yes |
| `PN-A2DP-0.1.0.34-testmode-portable.zip` | Extracted development tools, driver, licenses and codec sources | Yes, to load the PN driver |

The full portable archive does not make the kernel driver portable: the driver
still needs installation and a prepared test computer. The Windows-mode installer
preserves existing driver assignments. If a device already uses PN, select
**Driver > Use Windows mode** to restore Microsoft's driver for that device.

Windows mode provides output controls and diagnostics. Windows manages its codecs;
PN codec selection and Bluetooth bitrate telemetry need the custom driver.

## Verify the download

Download the installer, `SHA256SUMS.txt` and this guide from the same release.
For the custom driver, also download `PN-A2DP-Test-Certificate.cer`.
In PowerShell, open the download folder and compare each file's SHA-256 with the
matching entry in `SHA256SUMS.txt`:

```powershell
Get-FileHash .\PN-A2DP-0.1.0.34-testmode-setup.exe -Algorithm SHA256
Get-FileHash .\PN-A2DP-Test-Certificate.cer -Algorithm SHA256
```

The applications/installers currently have no public Authenticode publisher
signature, so Windows may show an unknown-publisher or reputation warning. The
driver itself has a development signature. These are different checks; an app
warning is not permission for Windows to load an unapproved kernel driver.

## Prepare a computer for the custom driver

Use a computer you can recover. Test Mode and disabling platform protections
reduce security. Windows mode is available without these changes. If BitLocker or
Device Encryption is enabled, have the recovery key available and follow your
PC/organization's procedure before firmware changes. Do not publish recovery keys.
Microsoft notes that Secure Boot and BitLocker can affect this boot setting.
[Microsoft Test Mode documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)

1. Save your work. In the PC's UEFI/BIOS settings, turn **Secure Boot OFF** using
   the manufacturer's instructions. Do not clear firmware keys or the TPM.
2. Open **Windows Security > Device security > Core isolation details** and turn
   **Memory integrity OFF**. Restart when required. This is the validated
   configuration required by **this PN development build**, not a rule that all
   test-signed drivers require Memory Integrity off.
3. Open **Terminal or PowerShell as administrator** and run:

   ```powershell
   bcdedit /set testsigning on
   ```

4. Restart Windows manually. The change is not active until after restart. Check
   the current boot entry with the following command; it should show
   `testsigning Yes`. A Test Mode desktop watermark may also appear.

   ```powershell
   bcdedit /enum '{current}'
   ```

If Windows reports that Secure Boot policy protects the setting, stop and check
the firmware setting. Do not use `nointegritychecks` or disable unrelated security
features. PN's **Driver > Windows / Test Mode...** dialog offers the same Test Mode
switch with current-session/next-start status; it never restarts automatically.
Close that dialog before driver activation.

## Verify and trust this release's test certificate

Certificate subject: `CN=PN-A2DP Test Certificate`. This release uses the same
development certificate as v0.1.0.30; if you already trusted it for that release, the
existing trust entries cover this one too.

- SHA-1 thumbprint (certificate identifier):
  `12EE1EAE30E0DEACCFB2870C393D3C148B1E210E`
- Certificate SHA-256 fingerprint:
  `EA99EBB435CCA73AD79B8927D4D4B187F720292137D5727C9FF4A38F828327DE`
- Expiration: **11 October 2031, 08:38:28 UTC**.

In administrator PowerShell, from the download folder, inspect the public
certificate before trusting it:

```powershell
Get-PfxCertificate .\PN-A2DP-Test-Certificate.cer |
    Format-List Subject, Thumbprint, NotAfter, HasPrivateKey
```

Its thumbprint must match above and `HasPrivateKey` must be `False`. The `.cer`
contains only the public certificate; no signing private key is distributed.
Trust it only if you intend to test this project. A root/publisher trust entry
applies to other packages signed with this certificate too.

```powershell
certutil -addstore Root .\PN-A2DP-Test-Certificate.cer
certutil -addstore TrustedPublisher .\PN-A2DP-Test-Certificate.cer
```

These commands use the **Local Machine** stores; do not add `-user`. Setup does
not import the certificate for you. Adding it alone does not enable normal-mode
loading of this development driver.
[Microsoft test-certificate guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/installing-test-certificates),
[certutil reference](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/certutil)

## Install and activate

1. **Stop playback** and close PN-A2DP Control Panel after saving or discarding edits.
   If audio is playing during installation, the old driver stays loaded until you
   restart Windows: `pnputil` cannot restart a device that is pending a restart.
2. Pair your receiver in Windows Bluetooth settings and keep it powered on. For a
   receiver with separate classic and BLE entries (for example the EDIFIER MR5), pair
   the classic entry; the BLE entry carries no audio.
3. From the download folder, launch the development installer explicitly:

   ```powershell
   .\PN-A2DP-0.1.0.34-testmode-setup.exe --allow-test-driver
   ```

4. Accept the administrator prompt if you intend to install. Keep the default
   **`C:\Program Files\PN-A2DP`** location, select the desired components and only
   the receiver(s) you want to switch. User-writable or redirected service folders
   are rejected. Double-clicking this installer without the developer option does
   not opt into installing its test-signed driver.
5. If Windows requests a restart, restart manually before judging the result.
6. Open the panel, select the receiver, and inspect **Driver**. For development
   activation, the requirements are **Test Mode ON**, **Secure Boot OFF** and
   **Memory Integrity OFF**. Amber/unknown or pending states must be resolved.
7. Select the receiver's output in Windows Sound settings, then play music and
   use **Quality > Check my audio**. Confirm sound, volume and mute yourself.
   Use **Quality > Events** to inspect observed bitrate changes. The optional
   **Use adaptive SBC** button is enabled only after persistent fixed-rate aptX
   trouble and sufficient fresh observations.

Setup changes neither boot policy nor firmware settings and does not reboot the
computer. Existing codec settings are retained on upgrade. A new Windows audio
endpoint ID can be assigned during installation; reselect the named receiver if
an application was pinned to its old endpoint.

Setup restarts the codec host service. If a receiver is playing through a host codec
(AAC, aptX, LDAC) at that moment, the driver falls back to SBC and several seconds of
audio can drop; the better codec returns at the next playback start.

While the development driver is installed, a receiver you pair later binds to the
PN-A2DP driver automatically, because its INF outranks Microsoft's by date. Use
**Driver > Use Windows mode** for any receiver that should stay on Microsoft's
driver. The panel's **Connect** and **Disconnect** buttons, like Windows Settings'
own buttons, connect or disconnect the selected receiver.

For inspection without installing, the development package supports:

```powershell
.\PN-A2DP-0.1.0.34-testmode-setup.exe --check --allow-test-driver --report .\pn-preflight.txt
```

This checks signature/payload and the current Test Mode policy; a successful
preflight is not a complete hardware or activation test. The Driver page performs
additional readiness checks.

## Return to normal Windows

1. Pause playback. For every receiver using the development driver, choose
   **Driver > Use Windows mode**; restart if requested. Alternatively, uninstall
   **PN-A2DP** through Windows **Installed apps / Apps & features**, which restores
   the inbox driver. Confirm Bluetooth playback works with the Microsoft driver.
2. Close the panel. If no other test-signed drivers need Test Mode, run this in
   administrator PowerShell, then restart manually:

   ```powershell
   bcdedit /set testsigning off
   ```

3. Restore Secure Boot and Memory Integrity using the same firmware/Windows
   settings, and follow the BitLocker procedure if protection was suspended.
   Restart as required. Check the resulting states; do not assume a saved toggle
   describes the running session.
4. If no remaining PN development installation needs this certificate, remove
   only these exact trust entries in administrator PowerShell:

   ```powershell
   certutil -delstore TrustedPublisher 12EE1EAE30E0DEACCFB2870C393D3C148B1E210E
   certutil -delstore Root 12EE1EAE30E0DEACCFB2870C393D3C148B1E210E
   ```

Disabling Test Mode before switching away from PN can leave its Bluetooth output
unavailable. If normal startup fails, use Windows Recovery's Startup Settings to
enter Safe Mode and uninstall PN-A2DP, then restart. This development build has
no Microsoft production signature; certificate trust or accepting a warning does
not change that.

## Tested and still unverified

On one Windows 11 / Qualcomm FastConnect 7800 machine:

- **Sony CMT-SBT40D (SBC only), driver 0.1.0.19:** SBC at **48 kHz / 672 kbps**, a
  15-second check without transport errors, and three reconnects of about
  **2.4 seconds** with the saved volume stable.
- **EDIFIER MR5 (SBC + LDAC), driver 0.1.0.20:** LDAC SQ (660 kbps) and **LDAC HQ
  (990 kbps)** for 30 seconds with no drops or write failures, three reconnects of
  about **0.3 seconds**, and no crackle in the owner's listening check.
- **Driver 0.1.0.21** installed without a restart with playback stopped. Its
  Connect / Disconnect support was tested by the owner and reported working; there is
  no automated log. Its playback was not re-measured, and the Sony was not re-tested
  after 0.1.0.19.

The software run passed all 34 verification checks, including 57,229 core checks
under AddressSanitizer, an end-to-end audio-quality harness (977 jobs, no defect
flags) and independent decoding of SBC, AAC and the aptX family.

AAC and the aptX family have software validation but no physical receiver
confirmation. LDAC independent decoding, confirmed physical-disconnect recovery,
sleep/wake, host-encoded hardware failure recovery, broader adapters/receivers,
Driver Verifier and HLK remain incomplete. Under heavy congestion dual-channel SBC
can drop to a distorted lowest step (about 132 kbps). No universal compatibility or
production readiness is claimed. Codec licenses and corresponding-source bundles
are included; those do not establish patent clearance or product certification.

## ภาษาไทยแบบย่อ

- ถ้าไม่ต้องการ Test Mode ให้ใช้ไฟล์ **windows-setup.exe**; Windows จะเป็นผู้เลือก codec
- ไฟล์ **testmode-setup.exe** เป็นไดรเวอร์ทดลอง ต้องเตรียม Test Mode ON, Secure Boot OFF
  และ Memory Integrity OFF ตามข้อกำหนดของรุ่นนี้ แล้วรีสตาร์ตและตรวจสถานะก่อนติดตั้ง
- ตรวจ hash/ลายนิ้วมือใบรับรองตามด้านบนก่อนเพิ่ม Root และ TrustedPublisher
- เปิดตัวติดตั้งด้วย `--allow-test-driver`; ตัวติดตั้งไม่ปิดระบบป้องกันและไม่รีสตาร์ตเอง
- หยุดเล่นเสียงก่อนติดตั้ง ถ้ายังเล่นเสียงอยู่ ไดรเวอร์เดิมจะยังทำงานจนกว่าจะรีสตาร์ต Windows
- อุปกรณ์ที่จับคู่ใหม่ระหว่างที่ติดตั้ง PN-A2DP อยู่ จะใช้ไดรเวอร์ PN-A2DP อัตโนมัติ; ใช้ Driver > Use Windows mode เพื่อย้ายกลับ
- ก่อนปิด Test Mode ให้เปลี่ยนทุกอุปกรณ์กลับเป็น Windows mode หรือถอน PN-A2DP ก่อน
- ถ้าเปิด BitLocker/Device Encryption ต้องเตรียม recovery key ก่อนแก้การตั้งค่า boot/firmware

For exact source and validation details, use the release tag in the repository:
[v0.1.0.34](https://github.com/patnawa/PN-A2DP/tree/v0.1.0.34).
