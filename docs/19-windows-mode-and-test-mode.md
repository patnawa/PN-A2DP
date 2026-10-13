# Windows mode and Test Mode: which one do I need?

A plain-language guide for application 0.1.0.34 and custom driver 0.1.0.21.
PN-A2DP can be used in two ways. You choose the way **per speaker or headphone**,
and you can change your mind later.

## The two ways in one paragraph each

**Windows mode** uses the Bluetooth audio driver that already comes with Windows,
made by Microsoft. PN-A2DP adds a control panel on top: volume and mute, the
active output and its Windows format, a **Check my audio** health check and a
diagnostic report. Windows still decides which codec and bitrate to use. Nothing
about how Windows starts is changed: no Test Mode, no certificate, and Secure Boot
and Memory Integrity stay as they are.

**PN driver mode** replaces Microsoft's Bluetooth audio driver with PN-A2DP's own
kernel driver for the speakers you pick. This unlocks codec choice (SBC, AAC-LC,
the aptX family and LDAC, when the receiver supports them), bitrate controls, the
live 60-second quality graph and transport diagnostics. Today this driver is
signed only with a **development certificate**, not by Microsoft, so Windows loads
it only when the PC is switched into **Test Mode** and prepared as described below.

## Side by side

| | Windows mode | PN driver mode |
|---|---|---|
| **What you get** | | |
| Bluetooth audio driver | Microsoft's built-in driver | PN-A2DP custom kernel driver |
| Codecs | Chosen by Windows | SBC, AAC-LC, aptX / aptX HD / aptX LL, LDAC, subject to receiver support |
| Bitrate and codec controls | Not available | Per-codec settings, priorities and automatic quality |
| Live quality (bitrate graph, drops, packet health) | Not available | Quality tab |
| Volume, mute, output information | Audio tab | Integrated with Windows audio controls |
| Check my audio and diagnostic export | Device and output checks | Adds codec, encoder, service and transport checks |
| Connect / Disconnect buttons | Yes | Yes |
| **What it needs** | | |
| Download | `windows-setup.exe` or `windows-portable.zip` | `testmode-setup.exe`, started with `--allow-test-driver` |
| Windows Test Mode | No | **Yes, ON** |
| Secure Boot | Leave as it is | **OFF** in this development build |
| Memory Integrity (Core isolation) | Leave as it is | **OFF** in this development build |
| Trust the PN-A2DP test certificate | No | Yes |
| Administrator rights | To install (not for the portable panel) | Yes, for setup and every driver switch |
| **Risk and impact** | | |
| Changes to Windows security settings | None | Test Mode on, two protections off, an extra trusted certificate |
| Maturity | Uses Microsoft's own driver | Experimental prerelease; limited hardware testing |
| If something goes wrong | Uninstall the app | A driver fault can affect audio or, in the worst case, Windows start-up; see [recovery](17-test-mode-release-guide.md#return-to-normal-windows) |

## Which should I choose?

- **You just want to control and check your Bluetooth audio:** Windows mode.
- **You use a work, school or otherwise managed PC:** Windows mode. Changing boot
  and security settings may be against your organization's policy.
- **You play games with anti-cheat, or use DRM-protected video apps:** Windows mode.
  Some of this software may refuse to run while Test Mode is on.
- **Your disk is protected by BitLocker or Device Encryption and you do not have the
  recovery key at hand:** Windows mode. Changing Secure Boot may make BitLocker ask
  for that key at the next start.
- **You want to choose the codec (for example LDAC), set bitrates or watch the live
  quality graph, and you are comfortable testing experimental software on a PC you
  can recover:** PN driver mode.
- **Not sure?** Start with Windows mode. You can install the Test Mode package and
  move a speaker to PN driver mode later.

## What Test Mode actually is

In normal mode, 64-bit Windows loads a kernel driver only if it has been signed
through Microsoft. Adding a certificate to the PC's trusted stores does not change
that. **Test Mode** (Windows calls it *test signing*) is a boot setting for
developers. When it is on, Windows also loads kernel drivers signed with a
**development (test) certificate**. It applies to the whole Windows installation,
not just to PN-A2DP, and it takes effect only after a restart.

While Test Mode is on, a **"Test Mode" watermark** may appear in the corner of the
desktop. That is Windows reminding you of the setting; it is not an error.

**Why PN-A2DP needs it today:** the PN driver is signed with the project's own
development certificate. Getting a Microsoft signature requires a Microsoft Hardware
Developer account, an EV code-signing certificate and Microsoft's testing and
submission process. That is not finished yet; see
[release readiness](15-commercial-readiness.md) and the
[Thai signing guide](11-production-signing-th.md). Accepting a Windows warning,
or trusting the certificate, does **not** replace a Microsoft signature.

**What a Microsoft-signed release would change:** the driver would install like
any normal driver. No Test Mode, no certificate import, and Secure Boot and
Memory Integrity could stay on. The panel already recognises a Microsoft-signed
PN package and then skips the development checks. Such a package does not exist
yet and no date is promised.

## What changes on your PC for PN driver mode

| Change | Why | Trade-off |
|---|---|---|
| Test Mode **ON** | Lets the development-signed PN driver load | Other test-signed kernel drivers can load too, not only PN-A2DP's. A desktop watermark may appear. Some anti-cheat or DRM software may refuse to run |
| Secure Boot **OFF** (in the PC's firmware/UEFI settings) | Secure Boot prevents Test Mode from being turned on | Less protection against malicious software that starts before Windows. BitLocker may ask for its recovery key |
| Memory Integrity **OFF** (Windows Security > Device security > Core isolation) | The configuration this development build has been tested with | Less protection against attacks on the Windows kernel |
| PN-A2DP test certificate trusted (Root and TrustedPublisher) | PN's installer and panel verify the driver package's signature before they install it or switch a speaker to it | That trust also applies to anything else signed with the same certificate. Only the public certificate is shipped; no private key is included |
| Custom kernel driver on the speakers you pick | Provides the PN features | It is experimental. A kernel driver fault can crash Windows; the driver has a crash guard that stops it from loading after two unclean starts in a row |

PN-A2DP's installer and panel **never** turn Secure Boot or Memory Integrity off,
never import the certificate for you, and never restart Windows by themselves. You
make those changes yourself, so you stay in control of them.

The **Driver** tab shows three lights for the running session: **Test Mode**,
**Secure Boot** and **Memory Integrity**. Green means the PN development driver's
requirement is met, red means it is not, amber means unknown or a pending change.
Green does not mean "safer": it only means "ready for this development driver".

## Switch a speaker between modes

After installation you switch each paired speaker separately in the control panel
(the Test Mode installer can also switch speakers during setup; see
[Turn Test Mode on safely](#turn-test-mode-on-safely)):

1. Stop playback and select the speaker in the device list.
2. Open the **Driver** tab.
3. Choose **Use Windows mode** to move it to Microsoft's driver, or
   **Use PN-A2DP driver** to move it to the PN driver. Accept the administrator
   prompt.
4. If Windows asks for a restart, save your work and restart yourself.

**Use PN-A2DP driver** is refused until Test Mode is ON (now and for the next
start), Secure Boot is OFF, Memory Integrity is OFF (running and saved), the PN
package's signature is trusted, and the **Windows / Test Mode...** window is
closed. **Requirements...** lists what is missing. **Use Windows mode** has no
such requirement. A speaker's PN codec settings are kept when you switch it to
Windows mode and come back when you switch it back.

While the PN driver package is installed, a speaker you pair **later** is put on
the PN driver automatically, because its driver ranks above Microsoft's. Use
**Use Windows mode** for any speaker that should stay on Microsoft's driver.
Windows Update can also put Microsoft's driver back on a
speaker; check the Driver tab if a speaker's features change unexpectedly.

## Turn Test Mode on safely

Do these steps in this order, on a PC you can recover. The detailed commands and
certificate fingerprints are in the [Test Mode release guide](17-test-mode-release-guide.md).

1. Save your work. If BitLocker or Device Encryption is on, have the recovery key
   ready (do not share it).
2. Turn **Secure Boot OFF** in the PC's firmware/UEFI settings, following the
   manufacturer's instructions. Do not clear firmware keys or the TPM.
3. Turn **Memory Integrity OFF** in Windows Security and restart if asked.
4. Turn **Test Mode ON**: in the panel, **Driver > Windows / Test Mode... >
   Enable Test Mode** (the button stays disabled while Secure Boot is on), or
   `bcdedit /set testsigning on` in an administrator terminal.
5. **Restart Windows** yourself. The dialog shows both the current session and the
   setting saved for the next start; the change counts only after a restart.
6. Verify and trust the test certificate, then run the Test Mode installer with
   `--allow-test-driver`. Setup refuses to install the driver unless Test Mode is
   already active. On the installer's device page, tick only the speakers you want
   on the PN driver. All paired speakers are ticked by default, and setup switches
   the ticked ones during installation. Unticked speakers keep Microsoft's driver.
7. Use **Use PN-A2DP driver** in the panel for any other speaker you want on the
   PN driver later (close **Windows / Test Mode...** first).

## Turn Test Mode off safely

**The order matters: move your speakers back to Windows mode *before* turning
Test Mode off.**

When Test Mode is off, Windows will not load a driver signed only with a
development certificate. A speaker still assigned to the PN driver would then
have no working driver, and no sound, until it is switched back.

1. Pause playback. For **every** speaker on the PN driver, including ones that are
   switched off or out of range, choose **Driver > Use Windows mode**. Restart if
   asked. Or uninstall PN-A2DP (see below), which does this for every speaker.
2. Check that sound works through Microsoft's driver.
3. Turn **Test Mode OFF**: **Driver > Windows / Test Mode... > Normal Windows (Test
   Mode OFF)**, or `bcdedit /set testsigning off` in an administrator terminal.
   Only do this if no other test-signed driver on the PC still needs Test Mode.
4. Restart Windows yourself.
5. Turn **Secure Boot** and **Memory Integrity** back on, restart as required, and
   check the lights or Windows Security to confirm.

The panel's Test Mode dialog helps you get the order right: it **refuses to turn
Test Mode off** while any paired speaker still uses a PN driver that is not
Microsoft-signed, and lists those speakers. Running `bcdedit` yourself has no such
check.

**If you already turned Test Mode off first** and a speaker has no sound: open the
panel, select that speaker and choose **Driver > Use Windows mode** (or uninstall
PN-A2DP). Alternatively turn Test Mode back on and restart. If Windows itself will
not start normally, use Safe Mode from Windows Recovery and uninstall PN-A2DP; see
[recovery](17-test-mode-release-guide.md#return-to-normal-windows).

## Remove everything

- **Windows mode installer:** uninstall PN-A2DP from **Settings > Apps >
  Installed apps** (listed as *PN-A2DP Bluetooth audio driver*). Nothing else
  needs undoing.
- **Windows-mode portable panel:** close it and delete its folder.
- **PN driver mode:**
  1. Uninstall PN-A2DP from **Installed apps** (listed as *PN-A2DP Bluetooth audio
     driver*). This restores Microsoft's driver on every speaker that used PN,
     removes the driver package from the Windows driver store, and removes the
     codec host service, shortcuts and files. Per-speaker PN settings may remain in
     the registry. Restart if asked and check that sound works.
  2. Turn Test Mode off and restart, then restore Secure Boot and Memory Integrity,
     as in [Turn Test Mode off safely](#turn-test-mode-off-safely).
  3. Remove the PN-A2DP test certificate trust entries if nothing else needs them;
     the exact commands are in the
     [Test Mode release guide](17-test-mode-release-guide.md#return-to-normal-windows).

## FAQ

**Do I need Test Mode for Windows mode?**
No. Windows mode never needs Test Mode, a certificate, or changes to Secure Boot or
Memory Integrity.

**Can some speakers use Windows mode and others the PN driver?**
Yes. The choice is per speaker, on the Driver tab.

**Can I keep Secure Boot on and still use the PN driver?**
Not with this development build. Secure Boot prevents Test Mode from being enabled,
and the panel refuses PN activation while Secure Boot is on.

**I clicked through the "unknown publisher" warning. Isn't that enough?**
No. That warning is about the installer application. Loading a kernel driver is a
separate, stricter Windows check; only Test Mode (today) or a Microsoft-signed
driver (in future) satisfies it.

**Is Test Mode dangerous?**
It lowers Windows' protection against untrusted kernel drivers, and this build
also asks you to turn Secure Boot and Memory Integrity off. That is a reasonable
trade-off on a personal test PC you can recover, and not a good idea on a PC that
holds sensitive work or is managed by an organization.

**How do I get rid of the "Test Mode" watermark?**
Follow [Turn Test Mode off safely](#turn-test-mode-off-safely). The watermark
disappears after the restart.

**A speaker I just paired uses the PN driver, but I wanted Windows mode.**
Select it and choose **Driver > Use Windows mode**.

**When will PN driver mode work without Test Mode?**
When a Microsoft-signed driver is released. There is no date yet.

## สรุปภาษาไทย

- **Windows mode** ใช้ไดรเวอร์ Bluetooth ของ Microsoft ที่มากับ Windows; PN-A2DP
  เพิ่มแผงควบคุมเสียง ปรับเสียง/ปิดเสียง ตรวจสุขภาพเสียง และส่งออกรายงาน Windows
  เป็นผู้เลือก codec เอง **ไม่ต้องเปิด Test Mode** ไม่ต้องติดตั้งใบรับรอง และไม่ต้อง
  ปิด Secure Boot หรือ Memory Integrity ใช้ไฟล์ `windows-setup.exe` หรือ
  `windows-portable.zip`
- **PN driver mode** ใช้ไดรเวอร์ของ PN-A2DP แทนไดรเวอร์ของ Microsoft เฉพาะลำโพงที่
  เลือก ได้การเลือก codec (SBC, AAC-LC, aptX, LDAC ตามที่ลำโพงรองรับ) ปรับ bitrate
  และกราฟคุณภาพสด แต่ไดรเวอร์รุ่นนี้เซ็นด้วยใบรับรองสำหรับพัฒนาเท่านั้น ยังไม่ได้เซ็น
  โดย Microsoft จึงต้องเปิด **Test Mode**, ปิด **Secure Boot**, ปิด **Memory Integrity**
  และเชื่อถือใบรับรองทดสอบก่อน แล้วรันตัวติดตั้ง `testmode-setup.exe --allow-test-driver`
- ในหน้าเลือกอุปกรณ์ของตัวติดตั้ง ให้เลือกเฉพาะลำโพงที่ต้องการใช้ไดรเวอร์ PN
  (ค่าเริ่มต้นเลือกไว้ทุกตัว และตัวติดตั้งจะเปลี่ยนไดรเวอร์ของลำโพงที่เลือกทันที)
- **Test Mode** คือการตั้งค่าตอนบูตของ Windows ที่อนุญาตให้โหลดไดรเวอร์ที่เซ็นด้วย
  ใบรับรองทดสอบ มีผลกับทั้งเครื่องหลังรีสตาร์ต อาจมีลายน้ำ "Test Mode" ที่มุมจอ
  โปรแกรม anti-cheat หรือ DRM บางตัวอาจไม่ทำงาน และ BitLocker อาจถาม recovery key
  หลังเปลี่ยน Secure Boot ถ้าในอนาคตไดรเวอร์ได้ลายเซ็นของ Microsoft จะไม่ต้องใช้
  Test Mode และไม่ต้องปิดระบบป้องกันอีก (ยังไม่มีกำหนด)
- ถ้าไม่แน่ใจ หรือใช้เครื่องของที่ทำงาน ให้เริ่มที่ **Windows mode**
- เปลี่ยนโหมดของแต่ละลำโพงที่แท็บ **Driver**: **Use Windows mode** หรือ
  **Use PN-A2DP driver** (ปุ่มหลังจะทำงานเมื่อ Test Mode ON ทั้งตอนนี้และหลังบูตครั้งถัดไป,
  Secure Boot OFF, Memory Integrity OFF, เชื่อถือลายเซ็นของแพ็กเกจ PN แล้ว และปิดหน้าต่าง
  **Windows / Test Mode...** อยู่ ดูสิ่งที่ยังขาดได้ที่ **Requirements...**)
- **ก่อนปิด Test Mode ต้องเปลี่ยนทุกลำโพงกลับเป็น Windows mode ก่อน** (หรือถอน
  PN-A2DP) ไม่อย่างนั้นลำโพงที่ยังใช้ไดรเวอร์ PN จะไม่มีเสียงจนกว่าจะเปลี่ยนกลับ
  หน้าต่าง **Windows / Test Mode...** จะไม่ยอมปิด Test Mode ถ้ายังมีลำโพงใช้ไดรเวอร์ PN
  อยู่ แต่คำสั่ง `bcdedit` ไม่ได้ตรวจให้
- ลำดับการปิด: ย้ายลำโพงกลับ Windows mode > ปิด Test Mode > รีสตาร์ต > เปิด Secure
  Boot และ Memory Integrity คืน > ลบใบรับรองทดสอบถ้าไม่ใช้แล้ว
- ตัวติดตั้งและแผงควบคุมไม่ปิด Secure Boot หรือ Memory Integrity ให้ ไม่ติดตั้งใบรับรองให้
  และไม่รีสตาร์ตเครื่องเอง (แผงควบคุมเปิด/ปิด Test Mode ได้เมื่อคุณกดยืนยันเท่านั้น)
- รายละเอียดคำสั่งและลายนิ้วมือใบรับรองอยู่ใน
  [คู่มือ Test Mode](17-test-mode-release-guide.md)
