# Bàn giao: ShieldCanLaserMC — Shield CAN Laser Marking MC (WinUI 3 C++, SOEM native)

Ngày lập: 2026-10-27
Phạm vi: thư mục `ShieldCanLaser.WinUI\` — phần mềm máy khắc laser Shield CAN, dựng từ bản WinUI 3 C++ của máy
`ShieldCanSlitAssyMC` (bản đang phát triển; bản WPF C# của máy Slit đã đóng băng, không dùng).
Đặc tả máy và chu trình: `..\PLAN-LASER.md`. Người đọc: lập trình viên tiếp tục phát triển và đưa app lên máy thật.

---

## 1. Kết luận

1. **Toàn bộ hạ tầng giữ nguyên từ máy Slit** — EtherCAT (SOEM native, luồng chu kỳ trên core CPU dành riêng), VisionMaster
   trong tiến trình, alarm, log, dữ liệu JSON, máy trạng thái, an toàn, phân quyền, khung giao diện 7 trang. Các mục
   4–6 dưới đây chép từ bàn giao máy Slit vì không đổi.
2. **Phần đặc thù máy viết lại theo PLAN-LASER.md §6**: 3 trục X/Y/Z (Panasonic MINAS A6B, mỗi driver một slave), 7 nozzle
   trên cụm X–Z, 2 tray 7 hàng × 7 cột trên Y, một camera cố định, laser kích bằng MỘT ngõ ra số + chờ `MarkTimeMs`
   (không giao tiếp, không gửi độ lệch — Tuấn chốt 2026-10-27). Bù độ lệch chỉ bằng trục X và Z, không có trục xoay.
3. **875 phép kiểm tự động đều đạt** (mục 7), chạy hoàn toàn mô phỏng, kể cả chu trình Auto đủ vòng (gắp hàng, soi từng
   nozzle, khắc đã bù, trả về tray, chờ thay tray) và chuỗi hiệu chuẩn (lưới X × Z, SET MASTER, thử laser).
4. **Chưa chạy trên phần cứng thật lần nào.** Bảng I/O (PLAN-LASER.md §5) là BẢN TẠM — chốt theo sơ đồ điện. Chiều Z
   (gốc ở trên, dương đi xuống như máy ShieldCanAssy) và dấu bù `GainX/GainZ` (mặc định −1) phải kiểm trên máy; đổi ở
   trang Setting, không sửa code.

---

## 2. Môi trường đã dùng

| Hạng mục | Giá trị |
|---|---|
| Hệ điều hành máy dev | Windows 10 Home 10.0.19045 |
| Visual Studio | VS 2026 Community, toolset v145 |
| Windows App SDK | 1.8, self-contained, unpackaged |
| Npcap | cài sẵn (`C:\Windows\System32\Npcap\wpcap.dll`) — thư viện nhập x64 kèm trong `ThirdParty\soem\oshw\win32\wpcap\Lib\x64` |
| VisionMaster | 4.4 kèm SDK C++ (chỉ cần header + thư viện nhập để build; chạy mô phỏng không cần) |
| Thư viện ngoài (kèm trong `ThirdParty\`) | SOEM v2.0.0 bản đã vá, nlohmann/json 3.11.3, pugixml 1.14 |

---

## 3. Cấu trúc

```
ShieldCanLaserMC\
├─ PLAN-LASER.md                 đặc tả máy, chu trình, điểm teach, hợp đồng vision, I/O tạm
├─ Config\alarms.json            bảng alarm (khối trục X/Y/Z, mã chu trình 5001–5006, 5020, 5030, 5031)
└─ ShieldCanLaser.WinUI\
   ├─ ShieldCanLaser.WinUI.sln   Soem + Core + CoreTests (app build riêng, xem mục 5)
   ├─ Soem\                      SOEM → thư viện tĩnh C
   ├─ Core\                      lõi máy → thư viện tĩnh C++20, KHÔNG phụ thuộc WinRT/XAML
   │  ├─ Base\, Utility\, SysConfig\, Infor\, Data\     hạ tầng (giữ nguyên) + AxisId/IoMap/Enums/AlarmCode, SystemData/ModelData/RunningData của máy Laser
   │  ├─ EtherCat\               thư viện EtherCAT (namespace ecm) — giữ nguyên
   │  ├─ Communication\          IAxis/IIoDevice/MotionConnect, EtherCatAxis/IoDevice, FakeAxis/FakeIo/FakeVision, VisionConnect, VmContract
   │  ├─ Controller\             McControl, McFunction, SafetyMonitor, Routine_Origin/Auto/Calib, VmCycle
   │  └─ Vision\                 thiết bị VisionMaster trên SDK C++ (một ảnh camera, kèm số nozzle)
   ├─ App\                       app WinUI 3 (C++/WinRT): MainWindow + Main/Manual/Teaching/Setting/EtherCAT/Vision Edit/Alarm
   ├─ Tests\CoreTests\           kiểm thử lõi (console) + đo jitter + thử VisionMaster thật
   ├─ ThirdParty\, DriverInfo\ (ESI Panasonic/Leadshine), tools\
   └─ build\                     đầu ra (không đưa vào kho mã)
```

Những chỗ đặc thù máy (đọc trước khi sửa):

| File | Nội dung |
|---|---|
| `Core\Infor\AxisId.h`, `IoMap.h`, `Enums.h`, `AlarmCode.h` | 3 trục; bảng DI/DO (`NozzleCount = 7`, `TrayCount = 2`); bước Auto S10–S60; mã alarm trục 2000/2100/2200, chu trình 5001–5006 |
| `Core\Data\SystemData.*` | `LaserComp` (GainX/GainZ/MaxCompMm/LaserPulseMinMs), `VisionCalib` (lưới X × Z, nozzle hiệu chuẩn), tên Flow/biến `VisionOutputs` |
| `Core\Data\Model\ModelData.*` | `AutoParams` (TrayRows, VacuumOn/OffMs, PickSlowLength, UseVision, UseLaser, MarkTimeMs); 8 điểm teach (`AutoPoints`), nội suy `PickPoint/CamPoint/LaserPoint`; `VisionCalib::Nozzles[7]` (Ref, Trim) |
| `Core\Data\RunningData.*` | `TrayRowsDone[2]`, `LastRowResult/Tray/Index` |
| `Core\Communication\VmContract.*`, `VisionConnect.*`, `Sim\FakeVision.*`, `GlobalVarSync.*`, `VmOutCheck.*` | một camera, một Flow `LocateCan`, biến `X_mm/Z_mm/Nozzle/RunId/IsLocate`, output `Found/Echo*/Obj*` |
| `Core\Controller\VmCycle.*` | dựng lời gọi Flow, đọc kết quả, độ lệch + đích bù (`Deviation`), kiểm tra lưới (`JudgeGrid`), cổng hiệu chuẩn |
| `Core\Controller\Routine_Auto.*` | chu trình PLAN-LASER.md §2; sơ đồ tray/nozzle cho trang Main (`Trays()`, `Nozzles()`, `ProgressText()`) |
| `Core\Controller\Routine_Calib.*` | lưới 9 điểm, VERIFY GRID, SET MASTER, GoToLaser/TestMark/SetTrim |
| `Core\Controller\McControl.*` | nút cứng START/STOP/RESET, đèn nút, `ResetTrays()` |

Quy ước code (giữ từ bản port): chuỗi `std::string` UTF-8; mọi thao tác dài là hàm CHẶN nhận `std::stop_token`, chạy
trên luồng nền; huỷ = `base::OperationCanceled`; sự kiện = `base::Event<T>`; giao diện không binding, đọc trạng thái
100 ms/lần; JSON nlohmann, cùng tên khoá giữa C# và C++.

---

## 4. EtherCAT: SOEM native và core CPU dành riêng

### 4.1 Vì sao phải đổi

Bản C++/CLI phát/nhận khung PDO trong callback của timer multimedia (`timeSetEvent`, 4 ms). Callback đó chạy trên một luồng của winmm: không ghim được core, chung CPU với UI, .NET, VisionMaster; và Windows 11 bỏ qua yêu cầu độ phân giải timer khi cửa sổ app bị che. Mỗi lần callback trễ vài ms là một lần driver thiếu khung — nguồn gốc lỗi "PDI Watchdog Timeout".

### 4.2 Thiết kế

| Thành phần | File | Việc |
|---|---|---|
| Dành riêng core | `Core\EtherCat\Soem\RtCpu.*` | Đặt CPU set mặc định của tiến trình = mọi CPU trừ core được chọn (mọi luồng của app, kể cả luồng do VisionMaster tạo sau này, tự rời core đó). Không cần quyền admin. |
| Luồng chu kỳ | `RtCyclicThread.*` | Luồng `THREAD_PRIORITY_TIME_CRITICAL`, ghim bằng CPU set **và** affinity cứng. Tắt EcoQoS và việc bỏ qua độ phân giải timer cho tiến trình. Giữ nhịp bằng `QueryPerformanceCounter`: ngủ bằng waitable timer độ phân giải cao tới sát hạn, rồi quay chờ đoạn cuối. Lỡ hạn thì bỏ nhịp và đồng bộ lại, không phát dồn. Thống kê bằng seqlock. |
| Vùng đệm process-image | `ProcessImage.*` | IOmap của SOEM thuộc RIÊNG luồng chu kỳ. Ngõ ra: luồng app xếp lệnh ghi vào hàng đợi vòng không khoá, luồng RT rút hết ngay trước khi phát khung. Ngõ vào: luồng RT chép sang bản chụp có số thứ tự, bên đọc đọc lại nếu trúng lúc đang ghi. |
| Bus SOEM | `SoemEtherCatBus.*` | Bring-up, dò PDO sống qua CoE, hook PRE-OP→SAFE-OP cho bộ ghép R1EC, sửa độ dài SyncManager, chẩn đoán khi không tới OPERATIONAL. Npcap nạp tường minh từ `System32\Npcap`; mọi DLL ngoài (Npcap, SDK VisionMaster) nạp trễ — máy thiếu chúng vẫn mở app ở chế độ mô phỏng. |
| Trần ưu tiên SDO | `SoemEtherCatBus.cpp`, `MailboxPriorityScope` | Mọi lời gọi mailbox tạm nâng luồng gọi lên TIME_CRITICAL trong lúc gọi (luồng SDO và luồng RT dùng chung critical section của nicdrv). |

Luồng quét trạng thái của `EtherCatMasterService` (4 ms, ưu tiên thường) chỉ đọc bản chụp, không chạm mạng.

### 4.3 Số đo (máy dev của bản Slit, chu kỳ 4 ms, việc giả 150 µs mỗi chu kỳ)

Chạy lại bằng `build\x64\Release\CoreTests\CoreTests.exe rtbench [giây] [số luồng tải] [cpu]`.

| Cấu hình | Không tải: lệch chu kỳ | 14 luồng tải: trễ so với hạn, tối đa |
|---|---|---|
| Timer multimedia (cách của bản cũ) | ±1400–1550 µs | lệch ±1000–1400 µs |
| Luồng RT ghim core, chỉ ngủ bằng timer | trễ tới 1000–1400 µs | 2300–7000 µs |
| Luồng RT ghim core, quay chờ 1 ms cuối | 220–570 µs | 9–55 µs |
| **Luồng RT ghim core, không ngủ (mặc định)** | **2–7 µs** | **0,8–15 µs** |

Core tự chọn: core số cao nhất thuộc lớp hiệu năng THẤP nhất (E-core trên CPU lai), không bao giờ là core chứa CPU 0.
Trên máy dev hiện tại (6 CPU) là CPU 5. Nhóm kiểm thử Realtime đòi trễ < 500 µs — máy dev đang bận thì thỉnh thoảng
lệch tới ~700 µs (chạy lại là đạt); đó là máy, không phải code.

### 4.4 Cấu hình (`Config\system.json`, trang Setting)

| Khoá | Mặc định | Ý nghĩa |
|---|---|---|
| `RtReserveCore` | `true` | Dành riêng core. `false`: luồng vẫn TIME_CRITICAL nhưng không ghim. |
| `RtCpuIndex` | `-1` | CPU logic dành riêng; -1 = tự chọn. |
| `RtCycleUs` | `4000` | Chu kỳ PDO (µs), kẹp trong 500–20000. |
| `RtSpinUs` | `-1` | Đoạn quay chờ trước mỗi hạn (µs); -1 = cả chu kỳ. |

### 4.5 Giới hạn

- CPU set chỉ điều khiển tiến trình này. Muốn cả Windows tránh core đó: khoá `ReservedCpuSets` trong
  `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\kernel` (cần admin, khởi động lại). Chưa thử.
- Khuyến nghị cho máy thật (chưa thử): power plan High/Ultimate Performance, tắt Interrupt Moderation, Energy-Efficient
  Ethernet, Green Ethernet và chế độ tiết kiệm điện của card EtherCAT.

---

## 5. Build và chạy

Điều kiện: VS 2026 có workload C++ desktop + Windows App SDK; VisionMaster 4.4 kèm SDK (cho header và thư viện nhập); Npcap (để chạy bus thật).

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb ShieldCanLaser.WinUI.sln -t:CoreTests -p:Configuration=Release -p:Platform=x64 -m      # Soem + Core + CoreTests
& $msb App\ShieldCanLaser.vcxproj -restore -p:Configuration=Release -p:Platform=x64 -m       # app
```

| Đầu ra | Đường dẫn |
|---|---|
| App | `build\x64\Release\App\ShieldCanLaser.exe` (cả thư mục là bản triển khai: kèm Windows App SDK, `Config\alarms.json`, `DriverInfo\`) |
| Kiểm thử | `build\x64\Release\CoreTests\CoreTests.exe` |

Tham số dòng lệnh của app:

| Lệnh | Tác dụng |
|---|---|
| `ShieldCanLaser.exe` | Chạy bình thường, dữ liệu ở `D:\ShieldCanLaser` (rơi về thư mục dự phòng nếu không ghi được). |
| `--root <thư mục>` | Dùng gốc dữ liệu khác, không giữ khoá một-bản-chạy. Để thử mà không đụng dữ liệu máy. |
| `--shot <thư mục>` | Đặt cửa sổ ngoài màn hình, không giành focus, chụp từng trang ra PNG rồi tự thoát. |

Bẫy build đã xử lý (đừng bỏ): `/EHs` cho mọi project gọi SDK VisionMaster; debug info `/Z7`; chỉ include SOEM qua
`Core\EtherCat\Soem\SoemInclude.h`; trong lớp implementation C++/WinRT viết `::base::…`. Thêm ở dự án này: thư mục
`x64` bị bỏ khi chép cây nguồn bằng robocopy /XD — thiếu `ThirdParty\soem\oshw\win32\wpcap\Lib\x64\wpcap.lib` thì
link báo LNK1104, chép lại từ máy Slit.

---

## 6. VisionMaster

| Phần | Cách làm | File |
|---|---|---|
| Lệnh (nạp solution, chạy Flow, biến toàn cục, đọc output, ảnh) | SDK C++ `iMVS-6000PlatformSDK`, trong tiến trình | `Core\Vision\VisionMasterDevice.*` |
| Trạng thái, khoá lệnh, canh giờ, alarm, log | `Core\Communication\VisionConnect.*` |
| Khung ảnh, trình soạn Flow | ActiveX trong popup sở hữu bám theo ô giữ chỗ | `App\Ui\VmPane.*` |

Hợp đồng Flow của máy này (PLAN-LASER.md §4.1): một Flow `LocateCan`; App ghi `X_mm`, `Z_mm`, `Nozzle`, `RunId` (float)
và `IsLocate` (int: 1 = soi, 0 = nạp điểm hiệu chuẩn) rồi chạy Flow; Flow trả `Found`, `EchoRunId`, `EchoX_mm`,
`EchoZ_mm` (bắt buộc) và `ObjFound`, `ObjX_mm`, `ObjY_mm`, `ObjRunId` (Script "CanAbs", tuỳ chọn). Nút CHECK VARS /
CHECK FLOW ở trang Setting đối chiếu solution với hợp đồng này. `CoreTests.exe vmprobe` thử thiết bị thật trên máy
có VisionMaster.

SDK C++ không tạo được biến toàn cục và không duyệt được IO từng module — như máy Slit; App báo rõ việc phải làm trong
VisionMaster.

---

## 7. Kiểm thử

`CoreTests.exe` (không cần phần cứng, mỗi nhóm dùng thư mục dữ liệu tạm):

| Nhóm | Số phép kiểm | Nội dung chính |
|---|---|---|
| Infra | 168 | IoMap/AlarmCode/AxisId 3 trục; system.json (3 trục A6B, LaserComp, VisionCalib) đọc-ghi, di trú mã driver cũ; model 8 điểm + nội suy hàng/CAM/laser; RunningData tray; alarms.json (mã 2205, 5004, 5006…); alarm, log, phân quyền |
| EtherCatSim | 67 | ConnectAllAxis, servo, chạy, jog, về gốc, I/O trên bus mô phỏng; ESI thật |
| Realtime | 37 | vùng đệm process-image; luồng RT ghim core, trễ < 500 µs |
| Motion | 172 | FakeAxis/FakeIo (7 van ↔ 7 cảm biến, đếm xung laser), MotionConnect 3 trục + Z_SAFE, đường EtherCAT (4 slave), các alarm kết nối |
| Vision | 152 | VmContract, VmOutCheck (Script CanAbs), 5 biến toàn cục, FakeVision (độ lệch từng nozzle, nhánh IsLocate), VmCycle (Deviation, JudgeGrid, EvaluateCalib), VisionConnect |
| Controller | 279 | khởi tạo, về gốc, AUTO/MANUAL, EMG/rèm/khí, thao tác tay, hiệu chuẩn (lưới, VERIFY, SET MASTER, GoToLaser/TestMark/trim), chu trình Auto đủ vòng (hút hụt, bù X/Z đúng số, vượt ngưỡng, vision NG, STOP = giữ tại chỗ, dừng ở checkpoint trả hàng về, chờ thay tray, RESET TRAY) |
| **Tổng** | **875** | đều đạt (2026-10-27) |

Giao diện: `ShieldCanLaser.exe --root <tạm> --shot <thư mục>` ở chế độ mô phỏng, xem ảnh từng trang.

---

## 8. Giao diện

| Trang | Nội dung |
|---|---|
| Main | 2 tray 7 × TrayRows ô (màu theo ECellState), dải 7 nozzle (giữ can / đang soi / đã khắc / NG, độ lệch dX dZ), ảnh camera, bước Auto, tiến độ hàng, bộ đếm, nút Auto/Manual/Start/Stop/Origin/Init, RESET TRAY |
| Manual | 3 trục (jog/inching/servo/home/move), 7 van chân không + cảm biến, đèn CAM, xung laser thử, bảng 32 DI / 32 DO, công tắc mô phỏng |
| Teaching | model, tham số Auto, tab Pick / CAM / Laser / Other, bảng điểm nội suy |
| Setting | 3 trục, Z_SAFE, cảm biến tuỳ chọn, vision (đường dẫn, tên Flow/biến, CHECK VARS/FLOW), LaserComp, RT |
| EtherCAT | giữ nguyên |
| Vision Edit | lưới 9 điểm X × Z + VERIFY, SET MASTER từng nozzle, thử laser + trim, cổng hiệu chuẩn |
| Alarm | giữ nguyên |

---

## 9. Đã kiểm chứng và chưa kiểm chứng

- Đã: mọi logic lõi bằng mô phỏng (mục 7); build app.
- Chưa: phần cứng thật (bảng I/O, chiều Z, dấu bù, thời gian khắc), solution VisionMaster thật cho Flow `LocateCan`,
  tốc độ/gia tốc thực của A6B, cữ hành trình.

---

## 10. Việc tiếp theo đề xuất

1. Chốt sơ đồ điện → sửa `Core\Infor\IoMap.h` và `Config\alarms.json` (tên X/Y), chạy lại CoreTests.
2. Trên máy: Origin → xác nhận chiều Z và Z_SAFE → teach 8 điểm → lưới X × Z → SET MASTER 7 nozzle → thử laser từng
   nozzle, chỉnh `GainX/GainZ` (dấu) và trim.
3. Dựng solution VisionMaster theo hợp đồng §4.1, kiểm bằng CHECK VARS / CHECK FLOW.
