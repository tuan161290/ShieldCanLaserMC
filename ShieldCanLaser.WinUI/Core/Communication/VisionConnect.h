#pragma once
#include "Communication/IVisionDevice.h"
#include "Communication/Sim/FakeVision.h"
#include "Data/SystemData.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace scs
{
    struct VisionCmdGate;   // khoá lệnh, sống lâu hơn VisionConnect khi còn một lần Run bị bỏ dở

    /// <summary>
    /// Camera CAM qua HIKROBOT VisionMaster 4.4.0. Tên Flow/output đọc từ <see cref="SystemData::VisionOutputs"/> (cấu
    /// hình được, không hardcode) — đó cũng là hợp đồng cần khớp khi dựng solution trong VisionMaster IDE
    /// (PLAN-LASER.md §4.1).
    ///
    /// <para>Lớp này KHÔNG biết SDK VisionMaster. Thiết bị thật là một <see cref="IVisionDevice"/> "thô" do
    /// hàm tạo đăng ký ở <see cref="SetRealDeviceFactory"/> cấp; VisionConnect bọc nó: kiểm trạng thái + thông báo, khoá
    /// lệnh, canh giờ <c>VisionTriggerTimeoutMs</c>, alarm 4003 dạng sự kiện, log và đổi exception thành kết quả thất
    /// bại. Thiết bị thô vì vậy: hàm CHẶN, không tự canh giờ, không phát alarm, lỗi SDK thì NÉM
    /// <see cref="VisionDeviceError"/> (what() = thông báo SDK).</para>
    ///
    /// <para>Bản giả (đọc lúc tạo): khi <c>SimulationMode</c> bật VÀ (<c>VisionSimulation</c> bật HOẶC chưa đăng ký
    /// hàm tạo) — mọi lời gọi đi THẲNG vào <see cref="FakeVision"/>, không qua khoá/canh giờ. Mô phỏng mà có hàm tạo và
    /// <c>VisionSimulation</c> tắt (mặc định): dùng VisionMaster thật như máy thật — trục/IO vẫn giả. <c>SimulationMode</c>
    /// tắt mà chưa đăng ký hàm tạo (hoặc hàm tạo hỏng): KHÔNG rơi về bản giả — log lỗi + alarm 4001, Connect/LoadSolution
    /// trả false.</para>
    /// </summary>
    class VisionConnect : public IVisionDevice
    {
    public:
        explicit VisionConnect(std::shared_ptr<SystemData> sys);

        /// <summary>Đăng ký nơi tạo thiết bị VisionMaster thật (thay cho <c>new VisionMasterDevice()</c>).
        /// Gọi một lần lúc khởi động app, trước khi Connect(). Truyền hàm rỗng để gỡ.</summary>
        static void SetRealDeviceFactory(std::function<std::shared_ptr<IVisionDevice>()> factory);
        static bool HasRealDeviceFactory();

        bool IsSimulation() const { return m_fake != nullptr; }
        /// <summary>Bản giả đang dùng (nullptr khi chạy thiết bị thật) — lớp điều khiển gắn FlowOutputSource,
        /// bài tự kiểm chỉnh tham số mô phỏng.</summary>
        std::shared_ptr<FakeVision> Fake() const { return m_fake; }
        /// <summary>Thiết bị đang dùng: bản giả, thiết bị thật, hoặc nullptr khi chưa tạo được thiết bị thật.</summary>
        std::shared_ptr<IVisionDevice> Device() const;

        bool IsConnected() const override;
        bool IsSolutionLoaded() const override;

        /// <summary>
        /// PlatformSDK không có bước "connect" tách rời khỏi nạp solution — kết nối camera thật diễn ra bên
        /// trong <c>LoadSolution</c>. Cờ này chỉ đánh dấu "sẵn sàng gọi nạp solution", đúng thứ tự
        /// <c>McControl.Init()</c> đang gọi: <c>Connect()</c> rồi <c>LoadSolution()</c>. Mô phỏng: nối bản giả
        /// và dựng kho biến giả theo <see cref="VisionGlobalVarCatalog"/>.
        /// </summary>
        bool Connect() override;
        void Disconnect() override;
        bool LoadSolution(const std::string& solutionPath) override;

        /// <summary>Kích Flow hiệu chuẩn kèm vị trí trục thật. Ghi biến toàn cục rồi Run trong cùng một lần gọi — xem
        /// <see cref="IVisionDevice::TriggerCalib"/>.</summary>
        VisionPose TriggerCalib(EVisionProcedure procedure, const AxisSnapshot& axes, std::stop_token ct) override;
        /// <summary>Không canh giờ (không chạy Flow), nhưng giữ khoá lệnh để không lọt lần kích Flow nào vào giữa.</summary>
        CalibClearResult ClearCalibPoints(EVisionProcedure procedure) override;
        GlobalVarSyncReport SyncGlobalVariables(const std::vector<GlobalVarSpec>& specs, bool apply) override;
        /// <summary>Không canh giờ như Run(): đây chỉ là đọc phần khai báo, không kích camera.</summary>
        VmFlowOutputs ReadFlowOutputs(const std::string& flow) override;
        VmCallResult CallFlow(const VmCallRequest& request, std::stop_token ct) override;
        std::string SaveSolution() override;

        std::shared_ptr<const ImageData> LastImage() const override;
        int LastImageNozzle() const override;

        // ── phần dựng lời gọi, để thiết bị thật dùng lại đúng một chỗ ──
        /// <summary>Việc nạp điểm → Flow chạy nó: GridCalib → LocateCan (nhánh hiệu chuẩn của chính Flow soi).</summary>
        static std::string FlowNameOf(const VisionOutputNames& names, EVisionProcedure procedure);
        /// <summary>Vị trí trục → tên biến toàn cục khai ở trang Setting. Tên để trống thì bỏ qua biến đó.</summary>
        static std::map<std::string, double> AxisVars(const VisionOutputNames& names, const AxisSnapshot& axes);
        /// <summary>Biến ghi trước một lần nạp điểm: <see cref="AxisVars"/> + biến rẽ nhánh = 0 (nhánh hiệu chuẩn).</summary>
        static std::map<std::string, double> CalibVars(const VisionOutputNames& names, const AxisSnapshot& axes);
        /// <summary>Nozzle (1…7) App ghi vào biến <c>Nozzle</c> của một lần gọi Flow; 0 nếu không ghi.</summary>
        static int NozzleOf(const VisionOutputNames& names, const std::map<std::string, double>& globals);

    private:
        std::shared_ptr<IVisionDevice> EnsureDevice(std::string& why);

        std::shared_ptr<SystemData> m_sys;
        std::shared_ptr<FakeVision> m_fake;
        std::shared_ptr<VisionCmdGate> m_gate;
        mutable std::mutex m_deviceMutex;
        std::shared_ptr<IVisionDevice> m_device;
        std::atomic<bool> m_connected{ false };
    };
}
