#pragma once
#include "Communication/IVisionDevice.h"
#include "Data/SystemData.h"

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>
    /// Vision mô phỏng — Flow kiểm tra của máy Laser Marking (PLAN-LASER.md §4). Mỗi lần gọi trả output <c>Result</c> của
    /// Group kiểm tra: 1 (OK) theo <see cref="PassRate"/>, hoặc đúng số đã đặt cho nozzle đó (<see cref="SetNozzleResult"/>)
    /// — để chạy được nhánh NG của chuỗi Auto bằng số biết trước.
    ///
    /// <para>Chỉ biết đúng một Flow và một Group: đường dẫn đang cấu hình (<see cref="SetNames"/>). Gọi tên khác thì báo
    /// không có, như solution thật.</para>
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
        /// <summary>Xác suất kiểm tra đạt (Result = 1) với nozzle chưa đặt số riêng.</summary>
        SCS_FAKE_PROP(double, PassRate, 1.0)
        /// <summary>Lần gọi kế tiếp báo lỗi (như Flow chạy hỏng) — kiểm App đánh ô NG và báo lý do.</summary>
        SCS_FAKE_PROP(bool, FailNextCall, false)
        /// <summary>Lần gọi kế tiếp chạy xong nhưng Group không có output Result.</summary>
        SCS_FAKE_PROP(bool, DropResultOnce, false)
#undef SCS_FAKE_PROP

        /// <summary>Result cố định cho nozzle k (1…7); nullopt = theo <see cref="PassRate"/>.</summary>
        void SetNozzleResult(int nozzle, std::optional<double> result);
        void ClearNozzleResults();

        /// <summary>Số lần đã chạy Flow kiểm tra.</summary>
        int InspectCount() const { return m_inspectCount.load(); }
        /// <summary>Số lần đã "lưu solution".</summary>
        int SolutionSaveCount() const { return m_solutionSaveCount.load(); }

        // Mô phỏng không có camera thật để chụp — luôn nullptr, trang Main tự hiện chỗ trống.
        std::shared_ptr<const ImageData> LastImage() const override { return nullptr; }
        int LastImageNozzle() const override { return m_lastNozzle.load(); }

        bool Connect() override;
        void Disconnect() override;
        bool LoadSolution(const std::string& solutionPath) override;

        std::string CheckModulePath(const std::string& path) override;
        /// <summary>Bị huỷ trong lúc chờ thì ném <c>base::OperationCanceled</c>.</summary>
        VmCallResult CallFlow(const VmCallRequest& request, std::stop_token ct) override;
        std::string SaveSolution() override;

        /// <summary>Nozzle của từng lần gọi, theo thứ tự (tối đa 5000 lần gần nhất).</summary>
        std::vector<int> CallHistorySnapshot() const;
        void ClearCallHistory();

        // ── móc nối (đặt trước khi dùng; đổi lúc đang chạy vẫn an toàn) ──
        /// <summary>Đường dẫn kiểm tra đang cấu hình — lớp trên gắn vào <c>SystemData.VisionOutputs</c>.
        /// Rỗng → bộ tên mặc định.</summary>
        void SetNames(std::function<VisionOutputNames()> names);
        /// <summary>Gọi sau khi Flow chạy xong, trước khi App đọc output — để bài test soi trạng thái máy lúc chụp.</summary>
        void SetAfterFlow(std::function<void(int nozzle)> afterFlow);

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

        VisionOutputNames Names() const;

        std::atomic<bool> m_connected{ false };
        std::atomic<bool> m_loaded{ false };
        std::atomic<int> m_inspectCount{ 0 }, m_solutionSaveCount{ 0 };
        std::atomic<int> m_lastNozzle{ 0 };

        mutable std::recursive_mutex m_lock;   // lịch sử, rng, móc nối, Result từng nozzle
        std::vector<int> m_callHistory;
        std::array<std::optional<double>, NozzleCount + 1> m_nozzleResult{};
        DotNetRandom m_rng;
        std::function<VisionOutputNames()> m_names;
        std::function<void(int)> m_afterFlow;
    };
}
