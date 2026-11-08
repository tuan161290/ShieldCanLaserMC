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
    /// Vision của máy Laser Marking chỉ KIỂM TRA sau khi khắc (Tuấn 2026-11-08: khách không cần bù độ lệch cho đầu khắc).
    /// Mỗi nozzle: App chạy Flow, đọc MỘT output <c>Result</c> của Group kiểm tra — 1 = OK, khác 1 = NG (PLAN-LASER.md §4).
    /// App không ghi biến toàn cục nào. Sửa ở trang Setting; VisionConnect đọc lúc chạy.
    /// </summary>
    struct VisionOutputNames
    {
        static constexpr int NozzleCount = 7;
        /// <summary>Tên output của Group mà App đọc — cố định, không cấu hình.</summary>
        static constexpr const char* OutResult = "Result";

        /// <summary>"Flow.Group": phần trước dấu chấm đầu tiên là Flow App chạy, cả chuỗi là đường dẫn Group đọc
        /// <see cref="OutResult"/>. Tên Flow VisionMaster 4.4 tối đa 16 ký tự.</summary>
        std::string InspectPath = "Flow1.GroupInspection";
        /// <summary>Output ảnh cấp Flow cho khung CAM trang Main; trống thì không lấy ảnh.</summary>
        std::string OutImage = "ImageData";
        /// <summary>Khung ảnh trang Main: "Flow" → cả Flow; "Flow.Module" → module đó; trống → Flow kiểm tra.</summary>
        std::string RenderCam;

        /// <summary>Flow của <see cref="InspectPath"/> (phần trước dấu chấm đầu tiên, bỏ khoảng trắng).</summary>
        std::string InspectFlow() const;
        /// <summary>Lý do <see cref="InspectPath"/> không dùng được; rỗng = dùng được.</summary>
        std::string InspectPathProblem() const;

        static std::string RenderPath(std::string_view flow, std::string_view path);
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

        // ── Laser: chỉ DO kích, không gửi gì sang đầu khắc ──
        /// <summary>Thời gian giữ DO kích laser tối thiểu (ms) — MarkTimeMs của model nhỏ hơn thì vẫn giữ đủ số này. PHỎNG ĐOÁN.</summary>
        int LaserPulseMinMs = 50;

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
