# ติดตั้ง PN-A2DP โดยไม่เปิด Windows Test Mode

ตรวจเอกสาร Microsoft วันที่ 12 ตุลาคม 2026

## สถานะของโครงการ

ไดรเวอร์ปัจจุบัน 0.1.0.16 ยังเซ็นด้วย **PN-A2DP Test Certificate** จึงยังใช้
ในโหมดปกติไม่ได้ การเปลี่ยนตัวติดตั้งหรือเซ็น EXE อย่างเดียวไม่เปลี่ยนข้อจำกัดนี้
ต้องนำไดรเวอร์ของเราไปผ่านกระบวนการเซ็นของ Microsoft ก่อน

ตัวติดตั้งใหม่ตรวจลายเซ็น catalog และ hash ของ INF/SYS ก่อนติดตั้ง ตรวจสายใบรับรอง
ด้วย Windows CryptoAPI และตรวจสิทธิ์การเซ็นไดรเวอร์ ไม่ตัดสินจากชื่อผู้เซ็นหรือการมี
ไฟล์ `.cat`/`.cer` เพียงอย่างเดียว ทั้ง wizard และ `--silent` ใช้นโยบายเดียวกัน
ตัวติดตั้งไม่เปิด Test Mode และไม่เพิ่ม test certificate เข้าเครื่อง

## 1. เตรียมบัญชีและใบรับรอง

### กรณีบุคคลธรรมดาในประเทศไทย

ผู้สมัครแจ้งว่าสมัครในนามบุคคลธรรมดาและเลือก Thailand
ถ้ามีเลขประจำตัวประชาชนไทยตามกฎหมายทะเบียนราษฎร เลข 13 หลักนั้นใช้เป็น
Tax Identification Number (TIN) ของบุคคลธรรมดา โดยไม่ต้องขอเลข TIN แยก
ดู [ข้อมูลประเทศไทยที่กรมสรรพากรเผยแพร่ผ่าน OECD](https://www.oecd.org/content/dam/oecd/en/topics/policy-issue-focus/aeoi/thailand-tin.pdf)
ข้อเท็จจริงด้านภาษีนี้ไม่ได้ยืนยันว่า Partner Center จะรับเลขดังกล่าวในทุกช่อง
โดยเฉพาะช่องที่ใช้ตรวจทะเบียนบริษัท ต้องตรวจประเภทผู้สมัครและชื่อหน้าฟอร์มก่อน
กรอกข้อมูลส่วนตัวบนเว็บไซต์ทางการโดยตรง ไม่ต้องส่งหมายเลขเข้าแชตหรือเก็บใน repo

ไม่ควรสรุปว่าต้องจดบริษัทเพื่อซื้อ EV ทุกกรณี: SSL.com มี
[Sole Proprietor EV Code Signing](https://www.ssl.com/products/software-integrity/code-signing/ev-sole-proprietor/)
ซึ่งผู้ให้บริการระบุว่ารองรับบุคคลธรรมดาโดยไม่ต้องมีบริษัทจดทะเบียน และใช้กับ
Windows Hardware Dev Center ได้ ต่างจากใบรับรอง IV Code Signing ธรรมดา
อย่างไรก็ตาม ยังไม่ได้ยืนยันการรับเอกสารไทยของผู้สมัครรายนี้หรือการอนุมัติบัญชี
Microsoft ควรยืนยันทั้งสองส่วนก่อนซื้อใบรับรอง

หากช่อง TIN บังคับข้อมูลของบริษัท หรือไม่พบตัวเลือกตรงกับสถานะบุคคลธรรมดา
ให้ใช้ช่องทาง Support ท้าย
[หน้าสมัคร Hardware Developer](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/hardware-program-register#support)
สอบถามวิธีสมัครที่ถูกต้อง ข้อความร่างสำหรับสอบถาม:

> I am an individual developer based in Thailand, with no incorporated company.
> I want to enroll in the Windows Hardware Developer Program to submit my own
> Bluetooth audio driver. The enrollment form requires a TIN registration number.
> Which enrollment path and verification documents apply to an individual or
> sole proprietor in Thailand? Can my Thai personal TIN and a Sole Proprietor EV
> Code Signing certificate be used? Please confirm before I purchase the certificate.

ร่างนี้ยังไม่ได้ส่งให้ Microsoft และไม่ต้องแนบหมายเลขประจำตัวในคำถามเบื้องต้น

### ข้อกำหนดบัญชี Hardware Developer

เตรียมข้อมูลองค์กรที่ตรวจสอบได้ ผู้ติดต่อที่มีอำนาจรับข้อตกลง อีเมลที่รับข้อความจาก
Microsoft และบัญชี Microsoft Entra ID ที่เป็น Global Administrator หากยังไม่มี
ไดเรกทอรี สามารถสร้างผ่านขั้นตอนลงทะเบียนได้

จัดหา **EV Code Signing Certificate** จากผู้ให้บริการที่ Microsoft ระบุ ตรวจเงื่อนไข
ยืนยันตัวตนและสิทธิ์สมัครขององค์กรกับผู้ให้บริการก่อนซื้อ บัญชี Hardware Developer
ต้องผูก EV ที่ยังใช้ได้ แม้การเซ็นไฟล์ส่งบางกรณีจะใช้ Authenticode certificate
ที่ลงทะเบียนกับบัญชีได้ เก็บ private key ในอุปกรณ์หรือบริการเซ็นของผู้ให้บริการ
ไม่ส่ง private key หรือรหัส PIN เข้าแชต

เริ่มที่ [การสมัคร Windows Hardware Developer Program](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/hardware-program-register)
และ [ข้อกำหนดใบรับรอง/รายชื่อผู้ให้บริการ](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs)
กรอกข้อมูลบริษัท ตรวจอีเมลผู้มีอำนาจรับข้อตกลง อัปโหลด EV ใน Manage certificates
ตอบแบบสอบถาม และรอการอนุมัติบัญชี

## 2. เลือกเส้นทางการเซ็น

| เป้าหมาย | เส้นทาง |
|---|---|
| แจกจ่ายเป็นผลิตภัณฑ์ทั่วไป | ทดสอบ Windows HLK แล้วส่งผ่าน WHCP พร้อม `.hlkx` |
| ทดสอบไดรเวอร์ที่ Microsoft เชื่อถือบน Windows Desktop | Attestation ตามเงื่อนไขสำหรับ testing scenarios ของ Microsoft |

Microsoft ระบุ Attestation สำหรับการทดสอบ และไม่ให้เผยแพร่ผ่าน Windows Update
สู่ผู้ใช้ทั่วไป ลายเซ็นนี้ไม่ใช่การรับรองว่าไดรเวอร์ผ่าน HLK สำหรับรุ่นแจกจ่ายควรใช้
WHCP/HLK และทดสอบกับ Windows รุ่นเป้าหมาย รวมถึง Secure Boot และ Memory integrity
ชุดทดสอบของ PN-A2DP ที่ผ่านในเครื่องนี้ไม่ทดแทนผล HLK
ดู [Driver signing options](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)

## 3. แพ็กเกจ CAB ที่เตรียมได้จากเครื่องนี้

```powershell
.\build\prepare-submission.ps1
```

สคริปต์ตรวจ INF และสร้างโฟลเดอร์ใหม่ใต้ `out/submission-*` พร้อม:

- `pna2dp-submission-UNSIGNED.cab` สำหรับเตรียมส่งตามเส้นทาง CAB/Attestation
- INF, SYS, CAT และ PDB ของ build เดียวกัน ภายในโฟลเดอร์ย่อย `PNA2DP`
- `submission-manifest.json` ระบุเวอร์ชัน ขนาด และ SHA-256

CAB นี้ **ยังส่งเป็นงานที่เซ็นเสร็จแล้วไม่ได้** และไม่ใช่แพ็กเกจรับรอง HLK
ไม่มี test certificate/private key อยู่ใน CAB ลายเซ็นทดสอบเดิมใน SYS/CAT
ไม่ทำให้เป็นรุ่นใช้งานปกติ Microsoft จะสร้าง catalog และเซ็นไดรเวอร์ที่ส่งกลับใหม่
ตาม [ขั้นตอน Attestation](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation)

หลังบัญชีอนุมัติและมีใบรับรองแล้ว สามารถใช้กระบวนการเซ็นของผู้ให้บริการ หรือ
สร้าง CAB ใหม่โดยระบุ thumbprint ของใบรับรองที่ลงทะเบียนกับ Partner Center:

```powershell
.\build\prepare-submission.ps1 -CertificateThumbprint '<thumbprint ของคุณ>'
```

สคริปต์ตรวจลายเซ็น CAB และ timestamp แต่ไม่อัปโหลดหรือยืนยันข้อตกลงแทนผู้ใช้
บน Hardware Dashboard ให้สร้าง submission ตามเส้นทางที่เลือก อย่าเลือกตัวเลือก
test-signing หากต้องการทดสอบการโหลดในโหมดปกติ สำหรับ WHCP ให้แนบผล HLK ตามที่
Dashboard กำหนดแทนการใช้ CAB นี้เป็นหลักฐานรับรอง

## 4. สร้างตัวติดตั้งจากไดรเวอร์ที่ Microsoft ส่งกลับ

แตกไฟล์ส่งกลับไว้ในโฟลเดอร์ที่มี `pna2dp.inf`, `pna2dp.sys`, `pna2dp.cat`
อย่าแก้ INF หรือคอมไพล์ SYS ทับหลังเซ็นแล้ว และอย่าใส่ `pna2dp-test.cer` ลงไป

```powershell
.\build\build.ps1 -Target dist -Production -DriverPackage 'C:\SignedDriver\PN-A2DP'
```

คำสั่งนี้ใช้ไดรเวอร์ที่ระบุโดยไม่คอมไพล์ใหม่ ตรวจแพ็กเกจด้วยตัวตรวจของ installer
และ `signtool verify /kp` ก่อนสร้างชุดแจกจ่าย ถ้าตรวจไม่ผ่านจะหยุดก่อนแทนที่ `dist`
ผลลัพธ์อยู่ที่ `out/Release/dist/pna2dp-setup.exe` ส่วน `-Production` หมายถึงผ่าน
เกณฑ์ลายเซ็นสำหรับแพ็กเกจ ไม่ได้อ้างว่าโครงการผ่าน HLK แล้ว

การตรวจ `/kp` ระบุ catalog ด้วย `/c` ให้ครอบคลุม INF และ SYS ทั้งคู่
ไม่บังคับให้ลายเซ็นฝังตัวลำดับแรกของ SYS ต้องเป็น Microsoft เพราะไดรเวอร์ที่มี
หลายลายเซ็นอาจวางลายเซ็นผู้พัฒนาไว้ก่อน Microsoft ได้ ดูผลตรวจจริงของ
[Alternative A2DP 1.8.3.1](12-alternative-signing-audit-th.md)

ควรเซ็น EXE/DLL ของผลิตภัณฑ์และตัว installer ด้วย Authenticode certificate
ของผู้เผยแพร่ด้วย การเซ็น EXE เป็นคนละขั้นตอนกับลายเซ็น kernel driver ของ Microsoft
และไม่ได้รับประกันว่าจะไม่มี SmartScreen prompt

ตรวจโดยไม่ติดตั้งได้ด้วย:

```powershell
.\out\Release\pna2dp-setup.exe --check --report 'C:\SignedDriver\preflight.txt'
```

## 5. เปลี่ยนเครื่องนี้กลับสู่โหมดปกติเมื่อแพ็กเกจพร้อม

ติดตั้งไดรเวอร์ Microsoft-signed ที่ตรวจผ่านก่อน ตรวจว่า Sony ใช้แพ็กเกจใหม่
และมี output endpoint จากนั้นจึงปิด Test Mode ด้วยสิทธิ์ administrator และ
รีสตาร์ตด้วยตนเอง ตรวจการเล่นเพลง ระดับเสียง mute การเชื่อมต่อซ้ำ และ codec
อีกครั้งในโหมดปกติ การทดสอบ Secure Boot/HVCI ต้องทำบนเครื่องที่ตั้งค่าเหล่านั้นจริง
เพราะลายเซ็นผ่านไม่ได้รับประกันความเข้ากันได้กับทุก security policy

ขณะนี้ยังไม่ได้ปิด Test Mode หรือเปลี่ยนไดรเวอร์ที่กำลังเล่นเพลง
การปิดตอนที่ยังใช้ไดรเวอร์ทดสอบอาจทำให้อุปกรณ์ PN-A2DP โหลดไม่ได้หลังรีสตาร์ต

สำหรับนักพัฒนาที่เตรียม test signing และความเชื่อถือใบรับรองไว้แล้วเท่านั้น:

```powershell
.\out\Release\pna2dp-setup.exe --allow-test-driver
```

ตัวเลือกนี้ไม่ใช่การติดตั้งโหมดปกติ และไม่เปลี่ยนนโยบาย boot ให้เอง
