#pragma once
#include "Communication/IVisionDevice.h"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace scs
{
    /// <summary>Một biến trong kho biến toàn cục giả: kiểu + giá trị viết theo InvariantCulture.</summary>
    struct FakeGlobalVar
    {
        std::string Type;
        std::string Value;
    };

    /// <summary>
    /// Vision mô phỏng — Flow <c>LocateCan</c> của máy Laser Marking. Trả vị trí can theo mm có nhiễu, kèm xác suất
    /// không tìm thấy để chạy được nhánh NG của chuỗi Auto.
    ///
    /// <para><b>Quang học giả:</b> can treo trên nozzle k lệch so với nozzle một lượng cố định
    /// (<see cref="SetNozzleDeviation"/>) — camera cố định nên <c>ObjX = X_mm + lệch X</c>, <c>ObjY = Z_mm + lệch Z</c>
    /// (cùng quy ước với file hiệu chuẩn By Coordinate: toạ độ vật lý theo hệ máy). Nhờ vậy độ lệch App tính ra đúng bằng
    /// lượng đã đặt, kiểm được phép bù ở trạm laser bằng số biết trước.</para>
    ///
    /// <para><b>Bản sao Script</b> (<see cref="CallFlow"/>): chạy đúng phép tính của Script trên kho biến toàn cục giả,
    /// nên bài tự kiểm kiểm được App đối chiếu echo và đọc output đúng với cùng những con số solution thật sẽ trả.</para>
    /// </summary>
    class FakeVision : public IVisionDevice
    {
    public:
        static constexpr int NozzleCount = 7;

        FakeVision();

        bool IsConnected() const override { return m_connected.load(); }
        bool IsSolutionLoaded() const override { return m_loaded.load(); }

#define SCS_FAKE_PROP(T, Name, init) \
        T Name() const { return m_##Name.load(); } \
        void Set##Name(T v) { m_##Name.store(v); } \
    private: std::atomic<T> m_##Name{ init }; public:

        /// <summary>Thời gian chụp + xử lý giả lập, ms.</summary>
        SCS_FAKE_PROP(int, ProcessTimeMs, 120)
        /// <summary>Xác suất tìm thấy đối tượng.</summary>
        SCS_FAKE_PROP(double, FoundRate, 1.0)
        /// <summary>Nhiễu đo, mm (±).</summary>
        SCS_FAKE_PROP(double, NoiseMm, 0.005)
        /// <summary>Lần LocateCan kế tiếp trả EchoRunId sai — bài test kiểm App bắt được đọc nhầm lần chạy.</summary>
        SCS_FAKE_PROP(bool, CorruptEchoOnce, false)
        /// <summary>Clear Point báo lỗi (như Flow không có module hiệu chuẩn) — kiểm chuỗi nạp điểm dừng trước khi chạy trục.</summary>
        SCS_FAKE_PROP(bool, FailClearPoint, false)
#undef SCS_FAKE_PROP

        /// <summary>Độ lệch giả của can trên nozzle k (1…7) so với vị trí mẫu, mm — 0 = can nằm đúng.</summary>
        void SetNozzleDeviation(int nozzle, double dx, double dz);
        std::pair<double, double> NozzleDeviation(int nozzle) const;

        /// <summary>Số lần đã trigger Flow soi — để bài tự kiểm kiểm chứng số lần chụp.</summary>
        int LocateCount() const { return m_locateCount.load(); }
        /// <summary>Số lần đã nạp điểm hiệu chuẩn (nhánh hiệu chuẩn) — kể cả lần soi lỡ ghi biến rẽ nhánh = 0.</summary>
        int CalibPointCount() const { return m_calibPointCount.load(); }
        /// <summary>Số lần đã bấm Clear Point, và số điểm nạp từ lần Clear Point gần nhất.</summary>
        int ClearPointCount() const { return m_clearPointCount.load(); }
        int PointsSinceClear() const { return m_pointsSinceClear.load(); }
        /// <summary>Số lần đã "lưu solution".</summary>
        int SolutionSaveCount() const { return m_solutionSaveCount.load(); }
        int VmCallCount() const { return m_vmCallCount.load(); }

        /// <summary>Vị trí trục kèm theo lần nạp điểm hiệu chuẩn gần nhất — kiểm App có gửi đúng toạ độ ENCODER hay
        /// không. nullopt = chưa nạp lần nào.</summary>
        std::optional<AxisSnapshot> LastCalibAxes() const;

        // Mô phỏng không có camera thật để chụp — luôn nullptr, trang Main tự hiện chỗ trống.
        std::shared_ptr<const ImageData> LastImage() const override { return nullptr; }
        int LastImageNozzle() const override { return m_lastNozzle.load(); }

        bool Connect() override;
        void Disconnect() override;
        bool LoadSolution(const std::string& solutionPath) override;

        /// <summary>Mô phỏng nạp một điểm hiệu chuẩn: ghi lại vị trí trục "gửi sang VisionMaster" rồi báo thấy /
        /// không thấy theo FoundRate. Bị huỷ trong lúc chờ thì ném <c>base::OperationCanceled</c>.</summary>
        VisionPose TriggerCalib(EVisionProcedure procedure, const AxisSnapshot& axes, std::stop_token ct) override;
        /// <summary>Đếm lần bấm, xoá bộ đếm điểm từ lần xoá trước; <see cref="FailClearPoint"/> thì báo lỗi.</summary>
        CalibClearResult ClearCalibPoints(EVisionProcedure procedure) override;

        GlobalVarSyncReport SyncGlobalVariables(const std::vector<GlobalVarSpec>& specs, bool apply) override;
        VmFlowOutputs ReadFlowOutputs(const std::string& flow) override;
        /// <summary>Bị huỷ trong lúc chờ thì ném <c>base::OperationCanceled</c>.</summary>
        VmCallResult CallFlow(const VmCallRequest& request, std::stop_token ct) override;
        std::string SaveSolution() override;

        // ── kho biến toàn cục giả: tên → (kiểu, giá trị) ──
        std::map<std::string, FakeGlobalVar> GlobalVarsSnapshot() const;
        /// <summary>Đặt thẳng một biến (tạo mới hoặc ghi đè cả kiểu) — bài tự kiểm dựng sẵn biến để kiểm luật
        /// "chỉ thêm, không sửa".</summary>
        void PutGlobalVar(const std::string& name, const std::string& type, const std::string& value);
        void RemoveGlobalVar(const std::string& name);
        void ClearGlobalVars();

        /// <summary>Dựng kho biến theo danh mục tạo biến.</summary>
        void SeedVmSolution(const std::vector<GlobalVarSpec>& specs);
        /// <summary>Đọc một biến giả (NaN nếu không có).</summary>
        double GetGlobal(const std::string& name) const;
        /// <summary>Ghi một biến giả giữ nguyên kiểu (không có biến thì bỏ qua).</summary>
        void SetGlobal(const std::string& name, double value);

        /// <summary>Thứ tự các Flow đã gọi (tối đa 5000 lần gần nhất).</summary>
        std::vector<std::string> FlowHistorySnapshot() const;
        void ClearFlowHistory();

        // ── móc nối (đặt trước khi dùng; đổi lúc đang chạy vẫn an toàn) ──
        /// <summary>Tên Flow/biến trục đang cấu hình — lớp trên gắn vào <c>SystemData.VisionOutputs</c>.
        /// Rỗng → bộ tên mặc định.</summary>
        void SetNames(std::function<VisionOutputNames()> names);
        /// <summary>Nguồn bản khai output khi mô phỏng — lớp điều khiển gắn vào <c>VmCycle.FakeFlowOutputs</c>;
        /// bài tự kiểm thay bằng bản có lỗi để kiểm tool báo đúng chỗ.</summary>
        void SetFlowOutputSource(std::function<VmFlowOutputs(const std::string&)> source);
        /// <summary>Gọi sau khi Flow chạy xong, trước khi App đọc output (và ngay lúc "chụp" của TriggerCalib,
        /// với tên EVisionProcedure) — để bài test chen một chuyển động vào / soi trạng thái đèn CAM.</summary>
        void SetAfterFlow(std::function<void(const std::string&)> afterFlow);

    private:
        class DotNetRandom
        {
        public:
            explicit DotNetRandom(int seed);
            double NextDouble();
        private:
            int m_seedArray[56]{};
            int m_inext{ 0 };
            int m_inextp{ 21 };
        };

        double Noise(double amplitude);
        double G(const std::string& name) const;
        int Gi(const std::string& name) const;
        void SetG(const std::string& name, double value);
        void RunLocateCan(const VisionOutputNames& n, std::map<std::string, double>& o);

        std::atomic<bool> m_connected{ false };
        std::atomic<bool> m_loaded{ false };
        std::atomic<int> m_locateCount{ 0 }, m_calibPointCount{ 0 };
        std::atomic<int> m_clearPointCount{ 0 }, m_pointsSinceClear{ 0 };
        std::atomic<int> m_solutionSaveCount{ 0 }, m_vmCallCount{ 0 };
        std::atomic<int> m_lastNozzle{ 0 };

        mutable std::recursive_mutex m_lock;   // kho biến, lịch sử, rng, móc nối, độ lệch
        std::map<std::string, FakeGlobalVar> m_globalVars;
        std::vector<std::string> m_flowHistory;
        std::optional<AxisSnapshot> m_lastCalibAxes;
        std::array<std::pair<double, double>, NozzleCount + 1> m_deviation{};
        DotNetRandom m_rng;
        std::function<VisionOutputNames()> m_names;
        std::function<VmFlowOutputs(const std::string&)> m_flowOutputSource;
        std::function<void(const std::string&)> m_afterFlow;
    };
}
