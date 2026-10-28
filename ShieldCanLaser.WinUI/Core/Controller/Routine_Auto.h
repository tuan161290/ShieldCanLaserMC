#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "Communication/IVisionDevice.h"
#include "Communication/MotionConnect.h"
#include "Controller/McFunction.h"
#include "Controller/McStatus.h"
#include "Controller/VmCycle.h"
#include "Data/Model/ModelData.h"
#include "Data/SystemData.h"
#include "Infor/IoMap.h"

namespace scs
{
    /// <summary>
    /// Chu trình chạy tự động — máy Laser Marking (PLAN-LASER.md §2).
    ///
    /// <para>Mỗi lượt là MỘT HÀNG tray: Y đưa hàng tới, Z hạ, 7 van bật, Z nâng (S20) → từng nozzle đang giữ can tới trước
    /// CAM, Flow LocateCan đo vị trí can, độ lệch = Rel − Ref[k] (S30) → từng nozzle còn OK tới điểm khắc đã bù X/Z, DO
    /// kích laser giữ MarkTimeMs (S40) → trả cả hàng về đúng ô (S50). Hết hàng của cả hai tray thì về vị trí chờ, chờ thay
    /// tray và bấm START (S60). Hút hụt / vision không thấy / độ lệch vượt ngưỡng → ô NG, can không khắc, vẫn trả về ô.</para>
    ///
    /// <para><b>Checkpoint</b> = sau khi trả hàng về tray (giữa các hàng) và trước bước CAM / bước laser (hàng đang trên
    /// nozzle thì trả về trước rồi mới dừng — không bao giờ dừng với can trên nozzle). STOP giữa lệnh trục: giữ nguyên
    /// chỗ, START chạy tiếp (bit RUN của McFunction). Lệnh khắc không bị cắt ngang.</para>
    ///
    /// <para>Quy tắc viết bước (kế thừa ShieldCanAssy): mọi lệnh trục đi qua <see cref="McFunction"/>
    /// (interlock + bit RUN), mọi chờ đợi observe <c>ct</c>.</para>
    /// </summary>
    class Routine_Auto
    {
    public:
        static constexpr int NozzleCount = IoMap::NozzleCount;
        static constexpr int TrayCount = IoMap::TrayCount;

        Routine_Auto(MotionConnect& motion, McFunction& func, McStatus& status,
                     std::function<std::shared_ptr<SystemData>()> sys, std::function<std::shared_ptr<IVisionDevice>()> vision);

        /// <summary>Yêu cầu dừng ở checkpoint kế tiếp (chuyển MANUAL, STOP lúc chờ…).</summary>
        std::atomic<bool> PauseRequested{ false };

        // ── Trao đổi với vòng quét McControl — nút cứng chỉ bắt được sườn ở vòng quét ──
        /// <summary>START vừa được bấm trong lúc chờ thay tray (vòng quét đặt, chu trình tiêu thụ).</summary>
        std::atomic<bool> TrayStartRequest{ false };
        /// <summary>Chu trình đang chờ thay tray — đèn START nhấp nháy, START = "đã thay tray".</summary>
        std::atomic<bool> WaitingTray{ false };

        /// <summary>Giây đã chạy của lượt (hàng) hiện tại — không tính lúc tạm dừng.</summary>
        double CurrentCycleSec() const;

        /// <summary>Số can đã xong của lượt hiện tại — Tack Time lượt đang chạy = CurrentCycleSec ÷ số này.</summary>
        int CurrentCycleProducts() const { return m_cycleDone; }

        /// <summary>Dừng/chạy lại đồng hồ cycle khi tạm dừng (STOP) và chạy tiếp.</summary>
        void HoldClock(bool hold);

        /// <summary>
        /// Chạy liên tục (HÀM CHẶN, luồng nền): hàng tiếp theo của tray còn hàng → hết tray chờ thay → lặp lại, tới khi được
        /// yêu cầu dừng. true = kết thúc bình thường (máy về Idle), false = dừng giữa chừng (máy về Paused). Bị huỷ có thể
        /// ném <c>base::OperationCanceled</c>.
        /// </summary>
        bool Run(std::stop_token ct);

        /// <summary>Một dòng cho trang Main: số hàng đã chạy của 2 tray và kết quả hàng vừa xong.</summary>
        static std::string ProgressText();

        /// <summary>
        /// Sơ đồ tray cho trang Main — ECellState từng ô, chu trình ghi trực tiếp lúc chạy. <see cref="Trays"/>: tray 1, 2,
        /// mỗi tray TrayRows × 7 ô theo hàng, ô = hàng × 7 + cột (Loaded = chưa chạy, Processing = hàng đang chạy, Ok / Ng =
        /// đã xong, Skipped = hàng đã xong từ phiên trước, Empty = tray vắng). Mảng rỗng / sai kích thước = chu trình chưa
        /// ghi gì từ lúc mở app — trang Main tự suy từ RunningData.
        /// </summary>
        struct TrayView
        {
            std::array<std::vector<int>, TrayCount> Trays;
        };
        static TrayView Trays();

        /// <summary>Trạng thái 7 nozzle cho trang Main: chỉ số 1…7 (phần tử 0 bỏ trống).</summary>
        struct NozzleView
        {
            /// <summary>ECellState: Empty = không giữ can, Loaded = đang giữ, Processing = đang soi/khắc, Ok = đã khắc, Ng.</summary>
            int State{ 0 };
            bool HasDeviation{ false };
            double Dx{ 0 }, Dz{ 0 };
            std::string Text;
        };
        static std::array<NozzleView, NozzleCount + 1> Nozzles();

    private:
        /// <summary>Stopwatch dừng/chạy tiếp được.</summary>
        struct CycleClock
        {
            void Restart();
            void Stop();
            void Start();
            void Reset();
            double Seconds() const;
        private:
            mutable std::mutex m_mutex;
            std::chrono::steady_clock::duration m_accum{};
            std::chrono::steady_clock::time_point m_since{};
            bool m_running{ false };
        };

        /// <summary>Một lượt = một hàng tray. Chỉ số 1…7 = nozzle (phần tử 0 bỏ trống).</summary>
        struct Row
        {
            int Tray{ 0 };    // 0-based
            int Index{ 0 };   // 0-based
            std::array<bool, NozzleCount + 1> Holding{};
            std::array<int, NozzleCount + 1> Result{};    // ECellState của từng ô
            std::array<bool, NozzleCount + 1> Seen{};     // vision đo được
            std::array<double, NozzleCount + 1> RelX{}, RelZ{};
        };

        bool PrepareStart(std::stop_token ct);
        bool NextRow(int& tray, int& row) const;
        bool WaitTrays(std::stop_token ct);
        bool RunRow(int tray, int row, std::stop_token ct);
        bool PickRow(Row& r, std::stop_token ct);
        bool Vision(Row& r, std::stop_token ct);
        bool Laser(Row& r, std::stop_token ct);
        bool PlaceRow(Row& r, std::stop_token ct);
        void Finish(const Row& r);

        /// <summary>Soi can trên nozzle k tại chỗ (đã ở điểm CAM): Rel đo được, hoặc lý do không thấy.</summary>
        bool Measure(int nozzle, double& relX, double& relZ, std::string& error, std::stop_token ct);
        /// <summary>Kích DO laser: ON, giữ MarkTimeMs (≥ LaserPulseMinMs), OFF — DO luôn về 0 kể cả khi bị huỷ.</summary>
        void Mark(std::stop_token ct);

        // ── tiện ích ──
        std::shared_ptr<ModelData> Model() const;
        std::shared_ptr<TeachPosition> PickAt(int tray, int row) const;    // 0-based
        std::shared_ptr<TeachPosition> CamAt(int nozzle) const;            // 1-based
        std::shared_ptr<TeachPosition> LaserAt(int nozzle) const;
        static double Coord(const TeachPosition& p, const char* axis);
        bool MoveAxes(const std::vector<std::pair<std::string, double>>& targets, std::stop_token ct);
        bool MoveZTo(double z, std::stop_token ct);
        bool TrayPresent(int tray) const;   // 0-based
        void SetStep(EAutoState step, const std::string& message);
        void EnterWait();
        void LeaveWait();
        IIoDevice& Io() const;

        MotionConnect& m_motion;
        McFunction& m_func;
        McStatus& m_status;
        std::function<std::shared_ptr<SystemData>()> m_sys;
        std::function<std::shared_ptr<IVisionDevice>()> m_vision;
        CycleClock m_cycleClock;
        std::atomic<int> m_cycleDone{ 0 };
        std::shared_ptr<ModelData> m_model;   // bản model chốt lúc bắt đầu Run
        std::string m_lastRowText;            // "Row done: …" — giữ ở đầu dòng nhắc lúc chờ
    };
}
