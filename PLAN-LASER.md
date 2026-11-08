# Shield CAN Laser Marking MC — thiết kế phần mềm

Ngày lập: 2026-10-27
Nguồn gốc: port từ `ShieldCanSlitAssyMC\ShieldCanSlitAssy.WinUI` (bản WinUI 3 C++20, SOEM native, Panasonic MINAS A6B —
bản đang được phát triển, 4 commit gần nhất đều ở bản này; bản WPF C# đã đóng băng). Toàn bộ hạ tầng giữ nguyên:
EtherCAT (luồng chu kỳ trên core riêng), VisionMaster (SDK C++ trong tiến trình), alarm, log, dữ liệu JSON, máy
trạng thái, an toàn, phân quyền, giao diện 7 trang. Chỉ phần **đặc thù máy** viết lại — liệt kê ở §6.

Mục này là đặc tả để viết code; chỗ nào là **PHỎNG ĐOÁN** (chưa có bản vẽ điện / cơ) đều ghi rõ, chốt lại trên máy thật.

---

## 1. Máy

| Phần | Mô tả |
|---|---|
| Trục | **X, Y, Z** — servo Panasonic MINAS A6B (EtherCAT, CiA402), bus SOEM. X và Z mang cụm 7 đầu hút; Y mang 2 tray. |
| Đầu hút | **7 nozzle N1…N7** trên một thanh cố định, bước giữa các nozzle = bước cột của tray ⇒ gắp cả HÀNG 7 can một lượt. Mỗi nozzle một van chân không (DO) + một cảm biến chân không (DI). Một Z chung. |
| Tray | **2 tray 7 hàng × 7 cột** trên trục Y (Tray 1, Tray 2), mỗi tray một cảm biến có tray. |
| Camera | **1 camera cố định** (CAM) — cụm X–Z đưa từng nozzle tới trước camera, kiểm tra từng can SAU KHI khắc. Đèn CAM là một DO. |
| Laser | **Không có giao tiếp** (Tuấn chốt 2026-10-27): App chỉ kích ON một DO rồi chờ `MarkTimeMs` (tham số model) là coi như khắc xong. |

**Vision chỉ kiểm tra (Tuấn chốt 2026-11-08).** Khách không yêu cầu bù độ lệch cho đầu khắc: can được khắc tại điểm teach
`Laser(k)` + trim của nozzle, rồi CAM kiểm tra kết quả khắc, App đọc MỘT output `Result` của Group kiểm tra (§4). Bản trước
(2026-10-27 → 11-07) soi trước khi khắc và bù X/Z — đã bỏ cùng hiệu chuẩn lưới 9 điểm, SET MASTER, biến toàn cục và
`LaserComp`.

## 2. Chu trình Auto (Routine_Auto)

```
S10 WaitStart     máy ở SẴN SÀNG AUTO, bấm START (có ít nhất một tray — cảm biến tray)
cho mỗi tray t = 1, 2 còn hàng, mỗi hàng r = 1…7 chưa chạy:
  S20 PickRow     Y → hàng r của tray t; X, Z → điểm gắp; Z hạ (hai chặng), 7 van bật, chờ, Z nâng;
                  đọc 7 cảm biến chân không → nozzle nào giữ can
  S30 Laser       với từng nozzle k đang giữ: X, Z → điểm Laser(k) + Trim[k]; DO kích ON; chờ MarkTimeMs; DO OFF
  S40 Inspect     với từng can vừa khắc: X, Z → điểm CAM(k); chờ ổn định; chạy Flow, đọc Result của Group
                  (Flow.Group ở Setting): 1 → ô OK, khác 1 → ô NG; không đọc được → ô NG + cảnh báo 5006
  S50 PlaceRow    Y, X, Z → đúng điểm gắp của hàng r; Z hạ; 7 van tắt; chờ; Z nâng → ô OK/NG
  hết hàng của 2 tray → S60 WaitTray: về vị trí chờ, đèn START nháy, chờ thay tray + bấm START
```

- Can luôn trả về **đúng ô** đã gắp (không có tray ra). Tray 1 xong rồi tới tray 2; tray vắng (cảm biến tắt) thì bỏ qua.
- NG: hút hụt → ô NG, không khắc, không kiểm tra; kiểm tra NG hoặc không kiểm tra được → ô NG (can đã khắc), trả về.
  Kiểm tra NG là ô NG thường, không phải alarm; chỉ khi Flow lỗi / quá giờ / thiếu `Result` mới có cảnh báo 5006.
- STOP giữa lệnh trục → giữ nguyên chỗ, START chạy tiếp; MANUAL → bỏ chu trình, phải Init. Điểm dừng (checkpoint): sau khi
  trả hàng và ngay sau khi gắp (hàng chưa khắc → trả về, lần sau chạy lại). Đã khắc thì kiểm tra và trả xong cả hàng rồi mới
  dừng — khắc lại là khắc đè. Đang khắc thì không dừng giữa lệnh khắc.
- Bật vision mà đường dẫn Flow.Group sai hoặc chưa nạp solution → dừng ngay lúc START, chưa khắc can nào.
- Tắt vision trong model (`UseVision = false`) → khắc xong là ô OK, không kiểm tra. Tắt laser (`UseLaser = false`) → chạy
  khô: đi đủ các điểm, không kích laser (vision bật vẫn kiểm tra — sẽ ra NG vì chưa khắc).
- **Cycle Time / Tack Time** (trang Main, bố cục và cách tính như ShieldCanAssy.WinUI). Một chu kỳ = một tray, hoặc một lượt
  chạy lẻ. Bốn dòng: Cycle Time (đồng hồ thực của chu kỳ đang chạy, tính cả lúc chờ và STOP, xong thì giữ số cuối tới khi chu kỳ
  mới bắt đầu), Last Cycle Time, Tack Time (Cycle Time ÷ số can OK), Last Tack Time. Sản lượng ước tính lấy từ Last Tack Time,
  chưa có thì lấy Tack Time.
- **Chạy lẻ (chọn ô).** Ở AUTO, chạm một ô trên sơ đồ tray trang Main = chọn ô đó; nhấn giữ rồi kéo sang ô khác cùng tray = cả
  vùng chữ nhật (như TrayCard của ShieldCanSlit); ô chọn viền 2 px, các ô khác mờ 0.35, nút `Clear (n)` bỏ chọn, như
  ShieldCanAssy.WinUI. Ô tray không có trạng thái trống: mặc định mọi ô Loaded, nền trắng, kể cả tray vắng. Có ô chọn ở bất kỳ tray nào thì chu trình CHỈ chạy các ô chọn, tray không chọn
  ô nào cũng không chạy. Vẫn mỗi lượt một hàng: chỉ van của ô chọn bật, chỉ nozzle đó khắc, kiểm tra và thổi nhả; Z chung nên các
  nozzle khác vẫn hạ theo, không bật van. Xong về vị trí chờ, máy về Idle, phải bấm RUN rồi START mới chạy tiếp. Khác
  ShieldCanAssy: ô không chọn vẫn là chưa chạy chứ không thành bỏ qua, lần chạy sau bỏ qua ô đã xong (kể cả trong cùng một
  hàng), vì đặt lại cả tray thì can đã khắc sẽ bị khắc đè. Ô đã chạy không chọn được (RESET TRAY trước), trừ lúc mọi tray có
  mặt đã xong hết: đó là chọn trước cho tray mới. Đang chạy thì khoá chọn. Ô chọn không lưu qua lần mở app.
- **Card Log trang Main** mặc định ẩn mỗi lần mở app. Thanh mảnh ở mép phải bật / tắt: card trượt ra và đẩy cột giữa, không đè
  lên, vì control VisionMaster ở khung CAM là cửa sổ riêng nằm trên XAML. Log vẫn cập nhật lúc ẩn.

## 3. Điểm teach (ModelData)

Mọi điểm chia đều từ ĐẦU tới CUỐI (như máy Slit — `ModelData::Interpolate`):

| Điểm | Trục | Ý nghĩa |
|---|---|---|
| `Pick Tray1 · Row 1`, `Pick Tray1 · Row 7` | X, Y, Z | nozzle 1 đúng trên cột 1 của hàng 1 / hàng cuối tray 1; Z = cao độ hút. Hàng giữa nội suy. |
| `Pick Tray2 · Row 1`, `Pick Tray2 · Row 7` | X, Y, Z | như trên, tray 2 |
| `CAM · N1`, `CAM · N7` | X, Z | nozzle 1 / nozzle 7 đúng giữa khung CAM (vị trí kiểm tra). N2…N6 nội suy. |
| `Laser · N1`, `Laser · N7` | X, Z | nozzle 1 / 7 đúng vị trí khắc (tâm trường laser). N2…N6 nội suy. |

Trim riêng từng nozzle (`Trim.Nozzles[k].X / Z`, giới hạn ±1 mm, model JSON khoá `"Trim"`) cộng vào điểm Laser(k) — chỉnh
tay ở trang Vision Edit sau khi xem mẫu khắc thử. Model cũ có khoá `"Calib"` thì chỉ lấy lại `TrimX / TrimZ`. Vị trí chờ = `StandbyPos` từng trục (Setting). Số hàng tray cấu hình (`TrayRows`, mặc định 7); số cột
cố định = 7 nozzle.

## 4. Vision và laser

### 4.1 Hợp đồng VisionMaster — kiểm tra sau khi khắc

Một đường dẫn **`Flow.Group`** cấu hình ở trang Setting (mặc định `Flow1.GroupInspection`): phần trước dấu chấm đầu tiên
là Flow App chạy, cả chuỗi là Group App đọc output. App không ghi biến toàn cục nào, không có Script bắt buộc.

| Output của Group | Kiểu | Ý nghĩa |
|---|---|---|
| `Result` | int hoặc float | **1 = OK**, khác 1 (0, −1…) = NG. Tên cố định trong code. |

- Mỗi can một lần chạy Flow tại điểm CAM(k), đèn CAM bật. Ảnh cho khung CAM trang Main đọc từ output ảnh cấp Flow
  (`ImageData`, Setting; trống = không lấy ảnh).
- SDK C++: chạy `IVmProcedure::Run()`, rồi `IMVSGroupTool::GetResult()->GetOutputInt/Float("Result")`. App bật callback kết
  quả của riêng Group đó trước khi chạy (LoadSolution tắt callback của mọi module). Group phải là module loại Group trong Flow
  (`GetAllModuleList`, `strModuleName` bắt đầu bằng `IMVSGroup`). **Chưa chạy được với solution thật trên máy dev.**
- Setting → **CHECK PATH**: tra Flow và Group trong solution đang nạp, không chạy Flow. SDK C++ không liệt kê được output
  của Group khi chưa chạy, nên `Result` chỉ kiểm được ở lần chạy đầu (thiếu → "missing output Result").
- Không đọc được `Result` (Flow lỗi, quá giờ `VisionTriggerTimeoutMs`, Group không chạy, thiếu output) → ô NG + cảnh báo
  5006 (dạng sự kiện), log ghi lý do.

### 4.2 Trang Vision Edit — thử khắc và thử kiểm tra

Không còn hiệu chuẩn camera. Cột phải trang Vision Edit có hai bước cho nozzle đang chọn:
1. **Thử khắc** — GO TO LASER (tới `Laser(k)` + trim, không kích), TEST MARK (kích DO tại chỗ), nhập trim X/Z rồi APPLY
   (ghi model). Danh sách 7 nozzle: trim và lần kiểm tra gần nhất.
2. **Kiểm tra** — hút một can đã khắc lên nozzle (trang Manual), GO TO CAM, INSPECT (chạy Flow tại chỗ, hiện `Result`).

### 4.3 (bỏ) Bù ở trạm laser

Đã bỏ 2026-11-08: không bù độ lệch can, chỉ có trim cố định từng nozzle (§3).

### 4.4 Kích laser

Không có truyền thông với laser: DO `LaserStart` lên 1, giữ `MarkTimeMs` (tham số model — theo nội dung khắc), về 0.
Thời gian giữ tối thiểu `SystemData::LaserPulseMinMs` ở Setting (phỏng đoán 50 ms) để laser bắt được sườn lên.

Lệnh khắc bị EMG/alarm cắt ngang thì `LaserStart` rớt và `LaserStop` phát một xung `LaserPulseMinMs`. Ba DI
`DiLaserReady`, `DiLaserBusy`, `DiLaserEnd` đã có chân, hiện ở trang Manual và `DiLaserReady`, `DiLaserBusy` lên đèn Laser ở thanh trạng thái, nhưng chu trình **chưa đọc**: vẫn chờ
theo thời gian như Tuấn chốt 2026-10-27.

## 5. I/O (theo danh sách của Tuấn 2026-11-03)

Remote IO Leadshine R1EC, module 32 DI rồi module 32 DO (như máy Slit). NC: 1 = bình thường. Máy không có nút RESET,
nút tắt còi hay cảm biến áp suất khí: RESET và tắt còi bấm trên màn hình.

| DI | Hằng số | Tín hiệu |
|---|---|---|
| X000 | `BtnStart` | Nút START |
| X001 | `BtnStop` | Nút STOP |
| X002 | `EmgOk` | EMG OK (NC) |
| X003 | `BtnStart2` | Nút START #2, cùng tác dụng X000 |
| X004 | `DiLaserReady` | Laser sẵn sàng (chu trình chưa đọc) |
| X005 | `DiLaserBusy` | Laser đang khắc (chu trình chưa đọc) |
| X006 | `DiLaserEnd` | Laser khắc xong (chu trình chưa đọc) |
| X007 | | dự phòng |
| X008, X009 | `TrayPresent(1…2)` | Cảm biến tray 1, 2 |
| X010 | `LightCurtainOk` | Rèm sáng OK (NC, tuỳ chọn) |
| X011…X017 | `VacuumSensor(1…7)` | Cảm biến chân không N1…N7 |
| X018 | `VacuumSensor(8)` | Dự phòng kênh 8 |
| X019…X031 | | dự phòng |

| DO | Hằng số | Tín hiệu |
|---|---|---|
| Y000 | `DoLampStartBtn` | Đèn nút START |
| Y001 | `DoLampStopBtn` | Đèn nút STOP |
| Y002 | `DoLampStartBtn2` | Đèn nút START #2, sáng cùng Y000 |
| Y003 | `LaserStart` | Kích khắc laser |
| Y004 | `LaserStop` | Dừng laser khi lệnh khắc bị cắt ngang |
| Y005 | | dự phòng |
| Y006…Y010 | `DoVacuum(1…5)` | Van chân không N1…N5 |
| Y011 | | dự phòng |
| Y012, Y013, Y014 | `DoTowerLampRed/Yellow/Green` | Đèn tháp đỏ, vàng, xanh |
| Y015 | `DoBuzzer` | Còi |
| Y016, Y017, Y019 | | dự phòng |
| Y018 | `DoLightCam` | Đèn CAM |
| Y020 | `ZAxisBrakeRelease` | Nhả phanh trục Z |
| Y021, Y022 | `DoVacuum(6…7)` | Van chân không N6, N7 |
| Y023 | `DoVacuum(8)` | Dự phòng kênh 8 |
| Y024…Y030 | `VacuumBlower(1…7)` | Van thổi N1…N7 |
| Y031 | `VacuumBlower(8)` | Dự phòng kênh 8 |

Hành vi gắn với các chân mới:

- **Phanh Z (Y020).** Vòng quét giữ ON khi và chỉ khi servo Z đang bật, kết nối tốt và không alarm. `McControl::ServoOff`
  đóng phanh, chờ `ZBrakeEngageMs` (100 ms, phỏng đoán) rồi mới tắt servo. Về gốc chờ `ZBrakeReleaseMs` (200 ms, phỏng
  đoán) sau khi nhả mới cho Z chạy. Đảo tay ở bảng DO bị vòng quét ghi đè.
- **Van thổi (Y024…Y030).** Lúc trả hàng về tray: van hút tắt, van thổi của nozzle đang giữ can bật trong suốt
  `VacuumOffMs`, tắt trước khi Z nâng. Nozzle hút hụt không thổi để khỏi hất can còn trong ô. START bị từ chối nếu còn van
  thổi đang mở.

## 6. Phần sửa so với máy Slit

| Khu vực | Việc |
|---|---|
| `Infor/AxisId.h` | 3 trục X, Y, Z; ZAxes = {Z}; PlanarAxes = {X, Y}; bỏ tool/rotate. |
| `Infor/IoMap.h` | bảng §5, `NozzleCount = 7`. |
| `Infor/Enums.h`, `AlarmCode.h` | bước Auto §2; mã trục X 2000 / Y 2100 / Z 2200. |
| `Data/SystemData` | tên máy, 3 trục A6B, đường dẫn kiểm tra Flow.Group §4.1, `LaserPulseMinMs`. |
| `Data/Model/ModelData` | điểm teach §3, `AutoParams`, `LaserTrim` (trim 7 nozzle). |
| `Data/RunningData`, `TrayCells` | `Trays[2]` (ô đã xong, ô chọn chạy lẻ), `LastRowResult`. |
| `Communication/VmContract`, `VisionConnect`, `Sim/FakeVision`, `IVisionDevice` | một camera; chạy Flow, đọc output của Group. |
| `Controller/VmCycle` | lời gọi kiểm tra, đọc `Result`. |
| `Controller/Routine_Auto`, `Routine_Calib`, `Routine_Origin`, `McControl`, `McStatus` | chu trình §2, nút thử §4.2, về gốc Z → X/Y. |
| `Vision/VisionMasterDevice` | một ảnh camera, nozzle thay tool, đọc output Group. |
| `Tests/CoreTests` | viết lại các nhóm theo máy mới (chạy hoàn toàn mô phỏng). |
| `App/*` | Main (2 tray 7×7, 7 nozzle, CAM), Manual (3 trục, 7 van, kích laser thử), Teaching (tab Pick / CAM / Laser / Other), Setting (bù), Vision Edit (§4.2). |
| `Config/alarms.json` | khối trục X/Y/Z. |

Không sửa: EtherCAT (Soem, Services, Esi, Transport), hạ tầng log/alarm/JSON/path/user, VmPane, AxHost, EtherCatPage,
AlarmPage, LoginDialog.
