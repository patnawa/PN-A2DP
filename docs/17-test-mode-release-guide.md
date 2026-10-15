# PN-A2DP 0.1.0.38: download, Test Mode setup and recovery

Application **0.1.0.38**, custom driver **0.1.0.25**, Windows x64.
This is an **experimental prerelease**, not a Microsoft-signed retail driver.

## Choose your package

| Download | Purpose | Test Mode required? |
|---|---|---|
| `PN-A2DP-0.1.0.38-windows-setup.exe` | Control panel using the existing Windows Bluetooth driver | No |
| `PN-A2DP-0.1.0.38-windows-portable.zip` | Windows-mode application without installation | No |
| `PN-A2DP-0.1.0.38-testmode-setup.exe` | Custom PN driver, codec host, control panel and codec plugins | Yes |
| `PN-A2DP-0.1.0.38-testmode-portable.zip` | Extracted development tools, driver, licenses and codec sources | Yes, to load the PN driver |

The full portable archive does not make the kernel driver portable: the driver
still needs installation and a prepared test computer. The Windows-mode installer
preserves existing driver assignments. If a device already uses PN, select
**Driver > Use Windows mode** to restore Microsoft's driver for that device.

Windows mode provides output controls and diagnostics. Windows manages its codecs;
PN codec selection and Bluetooth bitrate telemetry need the custom driver.
New to Test Mode? Read [Windows mode and Test Mode explained](19-windows-mode-and-test-mode.md)
first; it covers what Test Mode changes and which mode to choose.

## Verify the download

Download the installer, `SHA256SUMS.txt` and this guide from the same release.
For the custom driver, also download `PN-A2DP-Test-Certificate.cer`.
In PowerShell, open the download folder and compare each file's SHA-256 with the
matching entry in `SHA256SUMS.txt`:

```powershell
Get-FileHash .\PN-A2DP-0.1.0.38-testmode-setup.exe -Algorithm SHA256
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
development certificate as v0.1.0.30, v0.1.0.34 and v0.1.0.36; if you already trusted it
for one of those releases, the existing trust entries cover this one too.

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
   Close browsers too: Chromium-based browsers keep an audio stream open while paused.
   If audio is playing during installation, the old driver stays loaded until you
   restart Windows: `pnputil` cannot restart a device that is pending a restart.
2. Pair your receiver in Windows Bluetooth settings and keep it powered on. For a
   receiver with separate classic and BLE entries (for example the EDIFIER MR5), pair
   the classic entry; the BLE entry carries no audio.
3. From the download folder, launch the development installer explicitly:

   ```powershell
   .\PN-A2DP-0.1.0.38-testmode-setup.exe --allow-test-driver
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
7. Select the receiver's output in Windows Sound settings and choose its format under
   **Sound > the receiver > Properties > Advanced > Default Format** (drivers 0.1.0.23
   and later honour this choice; earlier PN drivers made Windows' shared-mode engine use the
   driver's first listed format, normally 48 kHz). Close and reopen the
   applications that play sound; Chromium-based browsers keep their audio stream open
   while paused and pick up the new format only after they reopen it. Then play music and
   use **Quality > Check my audio**. Confirm sound, volume and mute yourself.
   Use **Quality > Events** to inspect observed bitrate changes. The optional
   **Use adaptive SBC** button is enabled only after persistent fixed-rate aptX
   trouble and sufficient fresh observations.

Setup changes neither boot policy nor firmware settings and does not reboot the
computer on its own; the Finish page offers an optional, unticked "Restart the
computer now" box. Existing codec settings are retained on upgrade: a receiver configured
before this release keeps them (for example an LDAC SQ ceiling); use **Auto quality +
codecs** or the **Best quality** preset to move it to the new best-quality defaults, and
the **Transport** tab's **Music** listening profile for music. The new speaker delay
setting starts at **Automatic** for every receiver. A receiver that connects by itself now
gets its saved quality (driver 0.1.0.24 and later; before, the EDIFIER MR5 reconnecting on
its own ran LDAC at SQ under an HQ setting); this needs no action. A new Windows audio
endpoint ID can be assigned during installation; reselect the named receiver if
an application was pinned to its old endpoint.

**Speaker delay (lip-sync).** Players keep video in sync with the delay Windows reports
for the speaker. Driver 0.1.0.25 ignores delay reports too low to be real and lets you set
the speaker's delay. To measure it, stop playback everywhere and wait a few seconds, connect
a microphone that records at 16 kHz or more (allowed under **Windows Settings > Privacy &
security > Microphone**), place it 10 to 30 cm from the speaker in a quiet room, then open
**Headset > Speaker delay (lip-sync)**, choose the microphone and press **Measure with
microphone**. The speaker plays 8 short chirps over about 13 seconds; the recording is
analysed in memory and not saved. The result goes in as a **Fixed** value for the codec that
played; **Apply changes** saves it. A speaker's delay can change between stream starts (the
MR5's by about +-30 ms), so measure three to five times and type the middle value into the
**Fixed** box. The result includes the microphone's own timing (up to about 50 ms with the
USB headset microphone used in testing), so a value 20 to 30 ms below the middle one is a safe
choice. The command line equivalents are `pna2dpctl measure-delay -d N` and
`pna2dpctl set -d N sinkdelay=<ms> sinkdelay.codec=<codec>` (`sinkdelay=auto` to go back).
The setting changes only what Windows tells players; it does not delay the sound. Presets
keep it; the footer's **Defaults** button resets it to Automatic.

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
.\PN-A2DP-0.1.0.38-testmode-setup.exe --check --allow-test-driver --report .\pn-preflight.txt
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
  no automated log.
- **EDIFIER MR5, driver 0.1.0.23 (v0.1.0.36):** Windows opened the selected 44.1 kHz
  Default Format and the MR5 played **LDAC at 44.1 kHz natively** (909 kbps). Switching
  Windows to 24-bit, 48000 Hz during playback kept LDAC at 44.1 kHz with the driver's
  sample-rate converter bridging the rate, without a reconnect, and the owner heard no
  crackle. The receiver's own delay report (396 ms) was used for an end-to-end estimate
  of about 401 ms. The **Music** listening profile applied live without interruption.
  The session had no drops, write failures, latency trims or ring overflows. The
  driver installed without a restart with playback stopped (over the intermediate
  0.1.0.22 build; an upgrade from driver 0.1.0.21 was not tested). The intermediate driver
  0.1.0.22 had played LDAC HQ (990 kbps) at 48 kHz with no drops.
- **EDIFIER MR5, drivers 0.1.0.24 and 0.1.0.25 (this release; 0.1.0.24 was an
  intermediate build):** after a power cycle the MR5 set up the stream itself and it ran
  LDAC at **990 kbps** (driver 0.1.0.24; driver 0.1.0.23 had run such a stream at SQ,
  660 kbps, under an HQ setting). A PN-started stream on 0.1.0.24 also ran LDAC 48 kHz at
  990 kbps; no drops in steady streaming. The MR5 reported its delay as 25.0 ms right after setting up the stream and
  364.0 ms later, while a microphone (a USB-C headset microphone) measured **267-340 ms**
  over 10 stream starts, median about 315 ms, with one 693 ms outlier right after a
  browser had been playing. With a fixed 315 ms for LDAC, two checks heard the sound
  27.4 ms early and 16.8 ms late (before: 24-97 ms early with the 364 ms report, 268 ms
  late with the 25 ms report). The panel's **Measure with microphone** gave 306 ms and
  316 ms in two runs; the owner's first attempt showed no result, not explained. With its
  own 96 kHz setting on, the MR5 offers LDAC at 44.1, 48 and 96 kHz. Both drivers
  installed without a restart with playback stopped. The Sony was not re-tested after
  0.1.0.19.

The software checks for this release: 58,271 core checks; 151 checks of the speaker-delay
measurement's analysis on synthetic recordings (also under AddressSanitizer); and an
end-to-end audio-quality harness (1,252 jobs and 24 FFmpeg baselines, 1,276 in all; 0
defect flags and 0 global flags, with known issue #31 within its bounds; all 905 jobs at
the Windows rate bit-identical to the 0.1.0.21 baseline). Release run: `verify.ps1 -Sign
-Sanitize -Interop`, all 36 software checks passed (see the
[release notes](https://github.com/patnawa/PN-A2DP/blob/v0.1.0.38/docs/releases/v0.1.0.38.md)).

AAC and the aptX family have software validation but no physical receiver
confirmation. Not tested on hardware in this release: receiver-initiated AAC, aptX and
SBC configurations (SBC on the MR5 was measured for its delay only), the microphone measurement with any
other speaker or microphone, LDAC at 96 kHz, a single-step upgrade from v0.1.0.36, the Sony
on drivers 0.1.0.23 to 0.1.0.25, SBC, AAC and aptX through the sample-rate converter,
88.2 / 96 kHz Windows formats, the 48 to 44.1 kHz switch during playback, pause and resume
on a converted stream, the auto-start reconnect case (a first 44.1 kHz playback after a
48 kHz auto-start configuration), interference sessions with any profile, the Low latency
profile, the new-device defaults on a freshly paired receiver, the Transport tab's selector
and preset list on a real display, Secure Boot or Memory Integrity on, and long sessions.
A single speaker-delay measurement can be off by tens of milliseconds; use the middle of
several. LDAC independent decoding, confirmed physical-disconnect recovery,
sleep/wake, host-encoded hardware failure recovery, broader adapters/receivers,
Driver Verifier and HLK remain incomplete. Under heavy congestion dual-channel SBC
can drop to a distorted lowest step (about 132 kbps). No universal compatibility or
production readiness is claimed. Codec licenses and corresponding-source bundles
are included; those do not establish patent clearance or product certification.

## ภาษาไทยแบบย่อ

- อ่านคำอธิบายแบบเข้าใจง่ายว่า Windows mode กับ Test Mode ต่างกันอย่างไรได้ที่
  [Windows mode and Test Mode explained](19-windows-mode-and-test-mode.md#สรุปภาษาไทย)
- ถ้าไม่ต้องการ Test Mode ให้ใช้ไฟล์ **windows-setup.exe**; Windows จะเป็นผู้เลือก codec
- ไฟล์ **testmode-setup.exe** เป็นไดรเวอร์ทดลอง ต้องเตรียม Test Mode ON, Secure Boot OFF
  และ Memory Integrity OFF ตามข้อกำหนดของรุ่นนี้ แล้วรีสตาร์ตและตรวจสถานะก่อนติดตั้ง
- ตรวจ hash/ลายนิ้วมือใบรับรองตามด้านบนก่อนเพิ่ม Root และ TrustedPublisher
- เปิดตัวติดตั้งด้วย `--allow-test-driver`; ตัวติดตั้งไม่ปิดระบบป้องกันและไม่รีสตาร์ตเอง
- หยุดเล่นเสียงก่อนติดตั้ง ถ้ายังเล่นเสียงอยู่ ไดรเวอร์เดิมจะยังทำงานจนกว่าจะรีสตาร์ต Windows
- อุปกรณ์ที่จับคู่ใหม่ระหว่างที่ติดตั้ง PN-A2DP อยู่ จะใช้ไดรเวอร์ PN-A2DP อัตโนมัติ; ใช้ Driver > Use Windows mode เพื่อย้ายกลับ
- ตั้งแต่ไดรเวอร์ 0.1.0.23 ใช้รูปแบบเสียงที่เลือกใน Sound > อุปกรณ์ > Properties > Advanced > Default Format
  (รุ่นก่อนบังคับเป็น 48 kHz เสมอ) หลังเปลี่ยนให้ปิดแล้วเปิดโปรแกรมเล่นเสียงใหม่ เบราว์เซอร์ตระกูล
  Chromium (เช่น Brave) ต้องปิดแล้วเปิดใหม่จึงจะใช้รูปแบบใหม่
- อุปกรณ์เดิมยังใช้การตั้งค่าที่บันทึกไว้; กด Auto quality + codecs หรือเลือก preset Best quality
  เพื่อใช้ค่าคุณภาพสูงสุดใหม่ และเลือกโปรไฟล์ Music ในแท็บ Transport สำหรับฟังเพลง
- ลำโพงที่เชื่อมต่อเข้ามาเอง (เช่น หลังปิดแล้วเปิดลำโพงใหม่) จะใช้คุณภาพที่บันทึกไว้แล้ว เช่น LDAC HQ
  (ไดรเวอร์ 0.1.0.23 ใช้ LDAC SQ 660 kbps) ไม่ต้องตั้งค่าอะไรเพิ่ม
- ค่าหน่วงของลำโพง (Speaker delay) ใช้ให้ภาพกับเสียงตรงกันเวลาดูวิดีโอ ค่าเริ่มต้นคือ Automatic:
  ใช้ค่าที่ลำโพงรายงาน แต่ถ้าค่านั้นต่ำเกินจริง (เช่น EDIFIER MR5 รายงาน 25 ms) จะใช้ค่าเริ่มต้นของ codec แทน
- วัดค่าหน่วงจริงด้วยไมโครโฟน: หยุดเล่นเสียงทุกโปรแกรมแล้วรอสักครู่ ใช้ไมโครโฟนที่อัดได้ 16 kHz ขึ้นไป
  (อนุญาตใน Windows Settings > Privacy & security > Microphone) วางห่างลำโพง 10-30 ซม. ในห้องที่เงียบ
  แล้วไปที่แท็บ Headset > Speaker delay (lip-sync) เลือกไมโครโฟน กด Measure with microphone
  ลำโพงจะเล่นเสียงสั้น ๆ 8 ครั้ง ประมาณ 13 วินาที เสียงที่อัดจะวิเคราะห์ในหน่วยความจำและไม่ถูกบันทึกเป็นไฟล์
  ผลที่ได้จะใส่เป็นค่า Fixed ของ codec ที่กำลังเล่น กด Apply changes เพื่อบันทึก
- ค่าหน่วงของลำโพงบางรุ่นเปลี่ยนไปทุกครั้งที่เริ่มเล่นใหม่ (MR5 ต่างกันประมาณ ±30 ms) ควรวัด 3-5 ครั้ง
  แล้วพิมพ์ค่ากลางลงในช่อง Fixed ก่อนกด Apply changes (ผลวัดรวมความหน่วงของไมโครโฟนเองไว้ด้วย
  สูงสุดราว 50 ms จึงควรใส่ค่าต่ำกว่าค่ากลางประมาณ 20-30 ms); คำสั่งเทียบเท่าคือ `pna2dpctl measure-delay -d N`
  และ `pna2dpctl set -d N sinkdelay=<ms> sinkdelay.codec=ldac` (กลับเป็นอัตโนมัติด้วย `sinkdelay=auto`)
- ค่านี้เปลี่ยนเฉพาะค่าหน่วงที่ Windows แจ้งให้โปรแกรมเล่นวิดีโอทราบ ไม่ได้ทำให้เสียงช้าลง; preset ไม่เปลี่ยนค่านี้
  แต่ปุ่ม Defaults จะตั้งกลับเป็น Automatic
- ก่อนปิด Test Mode ให้เปลี่ยนทุกอุปกรณ์กลับเป็น Windows mode หรือถอน PN-A2DP ก่อน
- ถ้าเปิด BitLocker/Device Encryption ต้องเตรียม recovery key ก่อนแก้การตั้งค่า boot/firmware

For exact source and validation details, use the release tag in the repository:
[v0.1.0.38](https://github.com/patnawa/PN-A2DP/tree/v0.1.0.38).
