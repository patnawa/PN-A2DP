# ตรวจลายเซ็นและตัวติดตั้ง Alternative A2DP 1.8.3.1

ตรวจวันที่ 12 ตุลาคม 2026 จาก MSI บนเครื่องนี้ โดยเปิดฐานข้อมูลแบบ read-only
แตก CAB และตรวจไฟล์ด้วย Windows SDK ไม่มีการรัน MSI/custom action ติดตั้งไดรเวอร์
เปลี่ยน certificate store เปลี่ยนนโยบาย boot หรือรีสตาร์ตเครื่องในการตรวจรอบนี้

## ผลที่ยืนยันได้

Alternative A2DP ใช้ **ลายเซ็น Microsoft แบบ Attestation** สำหรับแพ็กเกจไดรเวอร์
และใบรับรอง **EV Code Signing ของ Luculent Systems, LLC ผ่าน SSL.com** สำหรับ
ตัวติดตั้งและโปรแกรม ประกอบกับ API ติดตั้งไดรเวอร์ปกติของ Windows
PN-A2DP ใช้กระบวนการเดียวกันได้เมื่อได้รับลายเซ็นของเราเองจาก Microsoft
การเปลี่ยนรูปแบบตัวติดตั้งเป็น MSI เพียงอย่างเดียวไม่ทำให้ผ่านการตรวจ kernel

ไฟล์ต้นทาง:

`%USERPROFILE%\Downloads\Programs\AlternativeA2dpSetup-1.8.3.1.msi`

ขนาด 6,115,328 bytes; SHA-256:

`dd8d7f5e52f3c7c6638b96fe6711f44e9a2121242a8de3811976d4d9958bea46`

หลักฐานที่สร้างไว้: `out/verification/alternative-signing-20261012/`
มี manifest ของไฟล์ที่แตกจาก MSI, ตาราง MSI, รายงานลายเซ็น, imports,
จุดเรียก API จาก disassembly และส่วนที่เกี่ยวข้องของ SetupAPI log

## 1. ลายเซ็นแต่ละชั้น

| ส่วน | สิ่งที่ตรวจพบ | ผล |
|---|---|---|
| MSI | Luculent Systems, LLC → SSL.com EV Code Signing Intermediate CA ECC R2 | `signtool verify /pa /all /v` ผ่าน |
| Config, DriverInstaller, Service EXE | ผู้เซ็นเดียวกับ MSI มี timestamp | ตรวจ Authenticode ผ่านทั้งสามไฟล์ |
| AltA2DP.cat ทั้ง x64/ARM64 | Microsoft Windows Hardware Compatibility Publisher → Microsoft Windows Third Party Component CA 2014 → Microsoft Root Certificate Authority 2010 | ตรวจ `/kp /c` กับ INF และ SYS ผ่านทั้งสองสถาปัตยกรรม |
| AltA2DP.sys ทั้ง x64/ARM64 | มี embedded signatures สองรายการ: index 0 เป็นบริษัท, index 1 เป็น Microsoft | `/pa /all` ตรวจผ่านสองลายเซ็น |
| binSetupDLL ที่ฝังใน MSI | ไม่มีลายเซ็นแยก | เป็นข้อมูลภายใน MSI ที่มีลายเซ็นแล้ว; ไม่ใช่ kernel driver |

ใบรับรองของ catalog มี EKU `1.3.6.1.4.1.311.10.3.5.1`
หรือ **Windows Hardware Driver Attested Verification** จึงระบุชนิดลายเซ็นได้จาก
ใบรับรองจริง ไม่ได้เดาจากคำว่า Hardware Compatibility Publisher

ไฟล์ x64 INF/SYS/CAT ที่แตกจาก MSI มี SHA-256 ตรงกับทั้งสามไฟล์ใน Driver Store:
`alta2dp.inf_amd64_f81fce6c1599eba4`

SetupAPI log เดิมบันทึกผู้เซ็นเป็น Microsoft Windows Hardware Compatibility
Publisher และ signer score เป็น WHQL เมื่อแพ็กเกจถูกนำเข้าเป็น `oem70.inf`
คำว่า WHQL ในคะแนนจัดอันดับนี้เพียงอย่างเดียวไม่ใช่หลักฐานว่าผ่านชุดทดสอบ HLK
รอบนี้ไม่ได้ทดลองโหลดใหม่ขณะปิด Test Mode

## 2. กระบวนการติดตั้ง

อ่านตาราง File, Component, Registry, CustomAction, InstallExecuteSequence,
LaunchCondition, ServiceInstall และ ServiceControl พบไฟล์ payload 18 รายการ
และ custom actions 21 รายการ

1. ตรวจ Windows รุ่น/สถาปัตยกรรม, Bluetooth radio, Bluetooth stack และ pending reboot
   ตัว MSI นี้ตั้งขั้นต่ำ x64 ที่ build 19045 และ ARM64 ที่ build 26100 แม้ INF
   จะมี section รองรับรุ่นเก่ากว่านั้น ตัวติดตั้งเลือกชุดไดรเวอร์ตาม `AaCpuArch`
2. คัดลอก GUI, helper, service และไดรเวอร์ ลงทะเบียน service ด้วย MSI
   มี custom actions สำหรับ prepare/install/upgrade/rollback/uninstall
   `caUpdateDriver` รันเมื่อเป็นการติดตั้งใหม่ที่ตรวจพบรุ่นเดิมเพื่ออัปเกรด
3. การเลือกไดรเวอร์ให้อุปกรณ์ทำผ่าน helper `AltA2dpDriverInstaller.exe`
   manifest ระบุ `requireAdministrator` และ GUI มีคำสั่ง `runas`
   จึงมีคำถาม UAC ให้สิทธิ์ administrator ตามปกติ
4. ทั้ง helper และ SetupDLL ใช้ `SetupCopyOEMInfW` และ `DiInstallDevice`
   พบ call sites ใน disassembly จริง: helper RVA `0x7E3C` และ `0x86C6`
   ส่ง driver info ของไดรเวอร์ที่เลือกให้ Windows ติดตั้งกับ device node
5. INF ระบุ MEDIA class, BTHENUM AudioSink UUID 0x110B, `PnpLockDown=1`,
   kernel service แบบ demand start และรัน SYS จาก Driver Store (`DIRID 13`)

ค่า flags `0x2` ที่ helper ส่งให้ `DiInstallDevice` คือ
`DIIDFLAG_NOFINISHINSTALLUI` ส่วนการเลือกไดรเวอร์เกิดจากการส่ง driver info
ไม่มี flag สำหรับข้ามลายเซ็นใน API นี้ ดู
[DiInstallDevice](https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-diinstalldevice)
และ [SetupCopyOEMInfW](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupcopyoeminfw)

ไม่พบคำสั่ง `bcdedit`, `testsigning`, `nointegritychecks` หรือการเพิ่มใบรับรอง
เข้า TrustedPublisher/SystemCertificates ในตาราง MSI หรือ strings/imports
ของ SetupDLL, helper, GUI และ service ที่ตรวจ ไม่มีไฟล์ `.cer` ใน File table
ข้อสรุปนี้เป็นผล static inspection ประกอบกับลายเซ็นที่ตรวจผ่าน ไม่ใช่การรับรอง
ทุกเส้นทางการทำงานของโปรแกรมจากการ trace runtime ทั้งหมด

## 3. จุดที่พบและแก้ใน PN-A2DP

ตัวตรวจ catalog ของ PN-A2DP รับแพ็กเกจ Alternative A2DP ทั้ง x64 และ ARM64 ได้:
`trusted=1 Microsoft=1 attestation=1` สำหรับ INF และ SYS ทั้งคู่
PN-A2DP ใช้ SetupCopyOEMInfW และ DiInstallDevice อยู่แล้ว จึงมีเส้นทางหลักเหมือนกัน

พบเงื่อนไขเกินจำเป็นใน `build/build.ps1 -Production`: หลังตรวจ catalog แล้ว
ยังบังคับ `signtool verify /kp pna2dp.sys` แบบไม่ระบุ catalog อีกครั้ง
คำสั่งนี้กับ Alternative A2DP 1.8.3.1 เลือก embedded signature แรกที่เป็นบริษัท
และคืน error ว่าสายใบรับรองไม่ได้จบที่ Microsoft แม้ catalog และลายเซ็น Microsoft
ลำดับที่สองถูกต้อง เป็น false rejection ของแพ็กเกจที่มีหลายลายเซ็น

แก้โดยคงการตรวจ Microsoft root/driver EKU/hash membership ของ installer และบังคับ
`signtool /kp /c` กับ INF และ SYS ทั้งสองไฟล์ ตัดการบังคับ embedded signature แรกออก
สอดคล้องกับ [ข้อกำหนดลายเซ็นของ PnP driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/embedded-signatures-in-a-driver-file)

ปรับชื่อ constant ของ `DiInstallDevice` ใน `tools/common/pndrv.cpp`
จาก `DIIRFLAG_FORCE_INF` เป็น `DIIDFLAG_NOFINISHINSTALLUI` ให้ตรง API
ค่าตัวเลขยังเป็น 0x2 เหมือนเดิม จึงไม่เปลี่ยนพฤติกรรมการติดตั้ง

เพิ่ม regression check ว่า catalog ของ Microsoft ที่เซ็นให้ Alternative A2DP
ต้องปฏิเสธ `pna2dp.sys` ของเรา ผลจริงคืน `0x800B0100`
ลายเซ็นผูกกับเนื้อหาไฟล์ จึงคัดลอก catalog/ลายเซ็นของเขามาใช้กับไดรเวอร์เราไม่ได้

## 4. สิ่งที่ PN-A2DP ยังต้องมี

| ขั้นตอน | สถานะ |
|---|---|
| เลือกอุปกรณ์และติดตั้งด้วย Windows API | มีแล้ว |
| ตรวจ catalog ของ Microsoft และไฟล์สมาชิก | มีแล้ว ทดสอบกับไฟล์จริงทั้ง x64/ARM64 |
| เตรียม CAB สำหรับส่งเซ็น | เตรียมแล้ว แต่ CAB ยังไม่เซ็น |
| EV certificate + บัญชี Hardware Developer | ผู้ใช้ยังไม่มี |
| ลายเซ็น Microsoft ของ PN-A2DP | ยังไม่มี ต้องส่งไดรเวอร์ของเราเอง |
| ลายเซ็นผู้เผยแพร่สำหรับ installer/EXE/DLL | ต้องทำเมื่อมีใบรับรอง |
| ทดสอบโหลด/เสียง/รีสตาร์ตใน normal mode, Secure Boot/HVCI | ยังไม่ได้ทำ ต้องมีแพ็กเกจพร้อมก่อน |

ในเชิงเทคนิค Attestation เป็นลายเซ็นที่ Windows Desktop เชื่อถือและไม่ต้องเปิด
Test Mode แต่เอกสาร Microsoft ปัจจุบันจัดเส้นทางนี้ไว้สำหรับ testing scenarios
และไม่อนุญาต Windows Update สำหรับ retail audience การตรวจไฟล์นี้ไม่บอกถึง
ข้อตกลงหรือประวัติ submission ของผู้พัฒนา Alternative A2DP
สำหรับผลิตภัณฑ์ PN-A2DP ที่จะแจกจ่ายทั่วไปให้วางแผน WHCP/HLK ตาม
[Driver signing options](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)

ขั้นตอนสมัคร/สร้าง CAB/นำไดรเวอร์ที่ Microsoft ส่งกลับมาสร้าง installer อยู่ใน
[คู่มือโหมดปกติ](11-production-signing-th.md)

## 5. ผลตรวจและขอบเขต

- การตรวจลายเซ็น 12 รายการได้ผลตามคาด รวม negative diagnostics สองรายการ
  ที่จงใจตรวจ embedded signature แรกด้วย `/kp`; ไม่ถือสองรายการนี้เป็น package failure
- ตัวตรวจ PN-A2DP รับแพ็กเกจจริงทั้งสองสถาปัตยกรรม และปฏิเสธ SYS ที่ไม่อยู่ใน catalog
- ชุดทดสอบ installer signing ครอบคลุมแพ็กเกจทดสอบ, catalog หาย/ถูกแก้ไข,
  INF/SYS ถูกแก้ไข, การปฏิเสธก่อนติดตั้ง และการปฏิเสธ catalog ของไดรเวอร์อื่น
- ไม่ได้รัน installer ของ Alternative A2DP หรือเปลี่ยนไดรเวอร์ที่กำลังใช้งาน
- การตรวจลายเซ็น ARM64 บนเครื่อง x64 นี้ไม่ใช่การทดสอบรันไดรเวอร์บน ARM64
