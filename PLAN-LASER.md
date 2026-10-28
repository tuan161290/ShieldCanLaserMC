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
| Camera | **1 camera cố định** (CAM) — cụm X–Z đưa từng nozzle tới trước camera, soi từng con một. Đèn CAM là một DO. |
| Laser | **Không có giao tiếp** (Tuấn chốt 2026-10-27): App chỉ kích ON một DO rồi chờ `MarkTimeMs` (tham số model) là coi như khắc xong. Không gửi độ lệch, không bù góc. |

**Hình học (Tuấn chốt 2026-10-27).** Camera và laser cùng nhìn vào mặt phẳng X–Z của can đang treo trên nozzle. Vision
đo độ lệch của can so với nozzle theo hai chiều X, Z; ở trạm laser App bù bằng trục X và Z. Máy không có trục xoay vuông
góc mặt phẳng (X, Z) nên không bù góc. Dấu/hệ số bù là tham số (§4.3) — chạy ngược thì đổi ở trang Setting, không sửa code.

## 2. Chu trình Auto (Routine_Auto)

```
S10 WaitStart     máy ở SẴN SÀNG AUTO, bấm START (có ít nhất một tray — cảm biến tray)
cho mỗi tray t = 1, 2 còn hàng, mỗi hàng r = 1…7 chưa chạy:
  S20 PickRow     Y → hàng r của tray t; X, Z → điểm gắp; Z hạ (hai chặng), 7 van bật, chờ, Z nâng;
                  đọc 7 cảm biến chân không → nozzle nào giữ can
  S30 Vision      với từng nozzle k đang giữ: X, Z → điểm CAM(k); chờ ổn định; gọi Flow LocateCan;
                  độ lệch (dX, dZ, dA) = (Obj − Enc) − Ref[k]; không thấy / vượt ngưỡng → ô NG
  S40 Laser       với từng nozzle k còn OK: X, Z → điểm Laser(k) + bù (dX, dZ); DO kích ON; chờ MarkTimeMs; DO OFF
  S50 PlaceRow    Y, X, Z → đúng điểm gắp của hàng r; Z hạ; 7 van tắt; chờ; Z nâng → ô OK/NG
  hết hàng của 2 tray → S60 WaitTray: về vị trí chờ, đèn START nháy, chờ thay tray + bấm START
```

- Can luôn trả về **đúng ô** đã gắp (không có tray ra). Tray 1 xong rồi tới tray 2; tray vắng (cảm biến tắt) thì bỏ qua.
- NG: hút hụt → ô NG, không soi/khắc; vision không thấy hoặc độ lệch vượt ngưỡng → không khắc, trả về.
- Checkpoint (STOP → giữ nguyên chỗ, START chạy tiếp; MANUAL → bỏ chu trình, phải Init): giữa các bước và giữa từng
  nozzle, như máy Slit. Đang khắc thì không dừng giữa lệnh khắc.
- Tắt vision trong model (`UseVision = false`) → khắc đúng điểm teach, không bù. Tắt laser (`UseLaser = false`) → chạy
  khô: đi đủ các điểm, không kích laser.

## 3. Điểm teach (ModelData)

Mọi điểm chia đều từ ĐẦU tới CUỐI (như máy Slit — `ModelData::Interpolate`):

| Điểm | Trục | Ý nghĩa |
|---|---|---|
| `Pick Tray1 · Row 1`, `Pick Tray1 · Row 7` | X, Y, Z | nozzle 1 đúng trên cột 1 của hàng 1 / hàng cuối tray 1; Z = cao độ hút. Hàng giữa nội suy. |
| `Pick Tray2 · Row 1`, `Pick Tray2 · Row 7` | X, Y, Z | như trên, tray 2 |
| `CAM · N1`, `CAM · N7` | X, Z | nozzle 1 / nozzle 7 đúng giữa khung CAM. N2…N6 nội suy. |
| `Laser · N1`, `Laser · N7` | X, Z | nozzle 1 / 7 đúng vị trí khắc (tâm trường laser). N2…N6 nội suy. |

Bù riêng từng nozzle (`Nozzles[k].TrimX / TrimZ`, giới hạn ±1 mm) cộng vào điểm Laser(k) — sửa tay sau khi kiểm tra
mẫu khắc. Vị trí chờ = `StandbyPos` từng trục (Setting). Số hàng tray cấu hình (`TrayRows`, mặc định 7); số cột
cố định = 7 nozzle.

## 4. Vision và laser

### 4.1 Hợp đồng VisionMaster — một Flow `LocateCan`

Như máy Slit: App ghi biến toàn cục rồi chạy Flow, Flow echo lại; rẽ nhánh soi / nạp điểm hiệu chuẩn bằng biến int.

| Biến App ghi | Kiểu | Ý nghĩa |
|---|---|---|
| `X_mm`, `Z_mm` | float | encoder X, Z lúc chụp |
| `Nozzle` | float | nozzle đang soi 1…7 (chỉ để log/hiển thị trong Flow) |
| `RunId` | float | số lần gọi, Flow echo lại |
| `IsLocate` | int | 1 = soi, 0 = nạp điểm hiệu chuẩn (module N-Point Calibration, By Coordinate = X_mm, Z_mm) |

| Output | Bắt buộc | Ý nghĩa |
|---|---|---|
| `Found`, `EchoRunId`, `EchoX_mm`, `EchoZ_mm` | có | thấy / không; echo |
| `ObjFound`, `ObjX_mm`, `ObjY_mm`, `ObjRunId` | tuỳ chọn | vị trí can trong hệ vật lý của file hiệu chuẩn (X ↔ trục X, Y ↔ trục Z) |

**Độ lệch** của nozzle k: `Rel = (ObjX − X_mm, ObjY − Z_mm)`; `Dev = Rel − Ref[k]` với `Ref[k]` là Rel đo lúc teach
("SET MASTER" ở trang Vision Edit, trung bình N ảnh, can mẫu lắp chuẩn). Vision tắt → Dev = 0. Không đo góc.

### 4.2 Hiệu chuẩn (trang Vision Edit)

1. **Lưới 9 điểm X × Z** — nozzle k (chọn) ngậm can mẫu, jog tới giữa khung, SET CENTER, RUN 9 POINTS (Clear Point rồi
   nạp 9 điểm với `IsLocate = 0`); tạo file hiệu chuẩn trong VisionMaster. VERIFY: chạy X +bước, Z +bước — `Rel` phải
   đứng yên trong dung sai (bắt sai thang đo, sai dấu, lẫn trục).
2. **SET MASTER nozzle k** — tới điểm CAM(k), chụp N ảnh, ghi `Ref[k]` vào model.
3. **Thử laser nozzle k** — tới điểm Laser(k) + bù theo lần soi gần nhất, không kích (hoặc TEST MARK có kích); nhập trim.

### 4.3 Bù ở trạm laser (`SystemData::LaserComp`)

```
X_target = Laser(k).X + TrimX[k] + GainX · dX          GainX mặc định −1
Z_target = Laser(k).Z + TrimZ[k] + GainZ · dZ          GainZ mặc định −1
|dX| hoặc |dZ| > MaxCompMm → ô NG, không khắc (cảnh báo 5004)
```

### 4.4 Kích laser

Không có giao tiếp với laser: DO `LaserTrigger` lên 1, giữ `MarkTimeMs` (tham số model — theo nội dung khắc), về 0.
Thời gian giữ tối thiểu `LaserPulseMinMs` ở Setting (phỏng đoán 50 ms) để laser bắt được sườn lên.

## 5. I/O (BẢN TẠM — chốt theo sơ đồ điện)

Remote IO Leadshine R1EC, module 32 DI rồi module 32 DO (như máy Slit). NC: 1 = bình thường.

| DI | Tên | | DO | Tên |
|---|---|---|---|---|
| X000 | START | | Y000 | đèn START |
| X001 | STOP | | Y001 | đèn STOP |
| X002 | EMG OK (NC) | | Y002 | đèn RESET |
| X003 | RESET | | Y003…Y009 | van chân không N1…N7 |
| X004 | rèm sáng OK (NC, tuỳ chọn) | | Y010/Y011/Y012 | đèn tháp đỏ/vàng/xanh |
| X005, X006 | cảm biến tray 1, 2 | | Y013 | còi |
| X007…X013 | cảm biến chân không N1…N7 | | Y014 | đèn CAM |
| X014 | áp suất khí OK (NC, tuỳ chọn) | | Y015 | kích laser |

## 6. Phần sửa so với máy Slit

| Khu vực | Việc |
|---|---|
| `Infor/AxisId.h` | 3 trục X, Y, Z; ZAxes = {Z}; PlanarAxes = {X, Y}; bỏ tool/rotate. |
| `Infor/IoMap.h` | bảng §5, `NozzleCount = 7`. |
| `Infor/Enums.h`, `AlarmCode.h` | bước Auto §2; mã trục X 2000 / Y 2100 / Z 2200. |
| `Data/SystemData` | tên máy, 3 trục A6B, `LaserComp`, tên Flow/biến §4.1, hiệu chuẩn lưới X × Z. |
| `Data/Model/ModelData` | điểm teach §3, `AutoParams`, `NozzleCalib[7]`, `GridVerifiedAt`. |
| `Data/RunningData` | `TrayRowsDone[2]`, `LastRowResult`. |
| `Communication/VmContract`, `VisionConnect`, `Sim/FakeVision`, `GlobalVarSync`, `IVisionDevice` | một camera, một Flow, biến X/Z/Nozzle. |
| `Controller/VmCycle` | yêu cầu gọi Flow, đọc kết quả, độ lệch, kiểm tra lưới, cổng hiệu chuẩn. |
| `Controller/Routine_Auto`, `Routine_Calib`, `Routine_Origin`, `McControl`, `McStatus` | chu trình §2, hiệu chuẩn §4.2, về gốc Z → X/Y. |
| `Vision/VisionMasterDevice` | một ảnh camera, nozzle thay tool. |
| `Tests/CoreTests` | viết lại các nhóm theo máy mới (chạy hoàn toàn mô phỏng). |
| `App/*` | Main (2 tray 7×7, 7 nozzle, CAM), Manual (3 trục, 7 van, kích laser thử), Teaching (tab Pick / CAM / Laser / Other), Setting (bù), Vision Edit (§4.2). |
| `Config/alarms.json` | khối trục X/Y/Z. |

Không sửa: EtherCAT (Soem, Services, Esi, Transport), hạ tầng log/alarm/JSON/path/user, VmPane, AxHost, EtherCatPage,
AlarmPage, LoginDialog.
