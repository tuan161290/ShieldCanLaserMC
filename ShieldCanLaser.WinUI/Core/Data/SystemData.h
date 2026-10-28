#pragma once
#include "Communication/AxisConfig.h"
#include "Communication/IoModuleConfig.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace scs
{
    /// <summary>
    /// Tên Flow (Procedure) và tên biến trong solution VisionMaster — máy Laser Marking có MỘT camera và MỘT Flow
    /// <c>LocateCan</c> (PLAN-LASER.md §4.1): soi can đang treo trên nozzle, rẽ nhánh soi / nạp điểm hiệu chuẩn bằng biến
    /// int <see cref="GvIsLocate"/> App ghi trước mỗi lần kích. Sửa tự do ở trang Setting; VisionConnect đọc các tên này lúc
    /// trigger. Tên Flow VisionMaster 4.4 tối đa 16 ký tự.
    /// </summary>
    struct VisionOutputNames
    {
        static constexpr int NozzleCount = 7;

        // ── Flow (Procedure) ──
        std::string FlowLocateCan = "LocateCan";   // CAM — soi can trên nozzle / nạp điểm N-point (X × Z)

        // ── Output chung ──
        std::string OutFound = "Found";        // 1.0 = tìm thấy, 0.0 = không
        std::string OutImage = "ImageData";    // trống thì không lấy ảnh

        // ── Khung ảnh trang Main: "LocateCan" → cả Flow; "LocateCan.Module" → module đó; trống → Flow soi ──
        std::string RenderCam;

        static std::string RenderPath(std::string_view flow, std::string_view path);

        // ── Biến toàn cục App ghi NGAY TRƯỚC mỗi lần kích Flow ──
        std::string GlobalVarModule;           // trống = App tự dò module đầu tiên đúng kiểu
        std::string GvAxisX = "X_mm";
        std::string GvAxisZ = "Z_mm";
        /// <summary>Nozzle đang soi 1…7 (float) — Flow chỉ dùng để hiển thị/log.</summary>
        std::string GvNozzle = "Nozzle";
        /// <summary>Biến rẽ nhánh, kiểu <b>int</b>: App ghi 1 trước mỗi lần soi, 0 trước mỗi lần nạp điểm hiệu chuẩn —
        /// trong CÙNG lời gọi với lần kích Flow. Trống = không ghi (Flow không rẽ nhánh).</summary>
        std::string GvIsLocate = "IsLocate";
    };

    /// <summary>
    /// Tham số các bước hiệu chuẩn vision ở trang Vision Edit (PLAN-LASER.md §4.2). Lưu trong system.json (khoá
    /// "VisionCalib"), không phải model: chúng tả cách đo, không tả sản phẩm.
    /// </summary>
    struct VisionCalibSettings
    {
        // ── Lưới 9 điểm X × Z ──
        double CenterX = 0, CenterZ = 0;    // tâm lưới = điểm 5 (Calibration Center Point)
        bool CenterSet = false;             // phân biệt "chưa lưu" với "lưu đúng tại (0, 0)"
        double GridStepX = 5.0, GridStepZ = 5.0;
        /// <summary>Nozzle ngậm can mẫu khi hiệu chuẩn lưới (1…7).</summary>
        int CalibNozzle = 1;

        // ── Kiểm tra lưới ──
        double VerifyStepMm = 3.0;

        // ── Chung ──
        int ShotsPerPoint = 5;              // số ảnh lấy trung bình ở các phép đo hằng số
        double ResidualTolMm = 0.05;        // dung sai kiểm tra lưới: Rel phải đứng yên trong ngưỡng này

        /// <summary>Danh sách số "-4, -2, 2" (dấu phẩy hoặc chấm phẩy); phần tử hỏng bị bỏ.</summary>
        static std::vector<double> ParseList(std::string_view text);
    };

    /// <summary>
    /// Bù ở trạm laser (PLAN-LASER.md §4.3). Dev = (dX, dZ) = độ lệch can so với can mẫu của nozzle, theo chiều máy
    /// (X ↔ trục X, Z ↔ trục Z). Laser không có giao tiếp và máy không có trục xoay — chỉ bù bằng hai trục này
    /// (Tuấn 2026-10-27). Dấu là tham số: chạy ngược thì đổi ở trang Setting, không sửa code.
    /// </summary>
    struct LaserCompSettings
    {
        /// <summary>X_target = Laser(k).X + TrimX[k] + GainX · dX.</summary>
        double GainX = -1.0;
        /// <summary>Z_target = Laser(k).Z + TrimZ[k] + GainZ · dZ.</summary>
        double GainZ = -1.0;
        /// <summary>|dX| hoặc |dZ| vượt ngưỡng → ô NG, không khắc (cảnh báo 5004).</summary>
        double MaxCompMm = 3.0;
        /// <summary>Thời gian giữ DO kích laser tối thiểu (ms) — MarkTimeMs của model nhỏ hơn thì vẫn giữ đủ số này. PHỎNG ĐOÁN.</summary>
        int LaserPulseMinMs = 50;
    };

    /// <summary>
    /// Tham số máy — lưu ở Config\system.json. Chỉ chứa thứ gắn với PHẦN CỨNG và cách vận hành, không
    /// chứa dữ liệu sản phẩm (ModelData).
    /// </summary>
    struct SystemData
    {
        std::string MachineName = "Shield CAN Laser Marking MC";
        std::string MachineCode;

        /// <summary>Chạy không cần phần cứng (FakeAxis/FakeIo; vision xem <see cref="VisionSimulation"/>). LUÔN
        /// hiện trên status bar.</summary>
        bool SimulationMode = true;

        /// <summary>Chỉ có tác dụng khi SimulationMode = true. false (mặc định): vision vẫn là VisionMaster THẬT —
        /// nó chỉ là phần mềm, không đụng trục/IO, nên chạy mô phỏng vẫn nạp, chỉnh và chạy được solution. true: vision
        /// giả (FakeVision, kết quả bịa) — dùng khi máy không cài VisionMaster hoặc muốn chạy thử Auto mà không có camera.</summary>
        bool VisionSimulation = false;

        /// <summary>Card mạng nối chuỗi EtherCAT, dạng Npcap \Device\NPF_{GUID}. "SIMULATED" = bus mô
        /// phỏng của thư viện EtherCAT. Chỉ có tác dụng khi SimulationMode = false.</summary>
        std::string NetworkAdapter;

        // ── Luồng chu kỳ EtherCAT (chỉ bus SOEM thật; xem EtherCat/Soem/RtCyclicThread.h) ──

        /// <summary>Dành riêng một core CPU cho luồng phát/nhận khung PDO. Tắt thì luồng vẫn chạy mức
        /// ưu tiên cao nhất nhưng chung core với phần còn lại của app.</summary>
        bool RtReserveCore = true;
        /// <summary>CPU logic dành riêng; -1 = tự chọn (core cuối, E-core nếu là CPU lai).</summary>
        int RtCpuIndex = -1;
        /// <summary>Chu kỳ PDO, micro giây. 4000 = giá trị đã chạy trên máy thật.</summary>
        int RtCycleUs = 4000;
        /// <summary>Đoạn quay chờ trước mỗi hạn, micro giây; -1 = cả chu kỳ (không ngủ, core chạy 100%,
        /// jitter thấp nhất). Giảm xuống (vd. 1000) để bớt điện/nhiệt, đổi lại đuôi trễ dài hơn.</summary>
        int RtSpinUs = -1;

        /// <summary>Remote IO R1EC — một module cố định. Không bao giờ null sau khi nạp.</summary>
        std::shared_ptr<IoModuleConfig> IoModule = std::make_shared<IoModuleConfig>();

        // ── An toàn / phần cứng tuỳ chọn ──
        double ZSafe = 10.0;                 // cao độ an toàn của Z (mm); Z dương là đi xuống
        bool ZSafeInterlock = true;          // X/Y chỉ chạy khi Z ở cao độ an toàn
        bool HasLightCurtain = false;
        bool HasAirSensor = false;           // DI AirOk
        bool HasTraySensors = true;          // DI TrayPresent 1, 2; tắt = coi như luôn có tray
        int ControlTickMs = 20;              // chu kỳ quét SafetyMonitor và vòng điều khiển
        int WatchdogTimeoutMs = 500;         // vòng điều khiển im quá lâu → alarm 1002
        int WatchdogStartupGraceMs = 5000;

        // ── Vision ──
        std::string VisionSolutionPath;      // file .solw nạp lúc khởi động
        int VisionTriggerTimeoutMs = 3000;
        int VisionSettleMs = 200;            // chờ cơ khí thôi rung rồi mới kích Flow
        int LampSettleMs = 100;
        VisionOutputNames VisionOutputs;
        VisionCalibSettings VisionCalib;
        int CalibValidDays = 90;             // quá hạn → cảnh báo 4090

        // ── Laser: chỉ DO kích + bù trục ──
        LaserCompSettings LaserComp;

        // ── Trục ──
        std::vector<std::shared_ptr<AxisConfig>> Axes;

        /// <summary>Tìm theo tên; chưa có thì tạo cấu hình mặc định, thêm vào Axes rồi trả về.</summary>
        std::shared_ptr<AxisConfig> GetAxis(std::string_view name);

        static std::shared_ptr<SystemData> CreateDefault();

        /// <summary>R1EC theo định danh trong ESI thật; vị trí slave mặc định ngay sau 3 trục (slave 3).</summary>
        static std::shared_ptr<IoModuleConfig> DefaultIoModule();

        std::string ToJsonText() const;
        /// <summary>nullptr khi nội dung là null/rỗng; ném khi JSON hỏng. KHÔNG chạy bước sắp lại trục
        /// (việc đó của McDataManager::LoadSystemData).</summary>
        static std::shared_ptr<SystemData> FromJsonText(const std::string& text);
    };
}
