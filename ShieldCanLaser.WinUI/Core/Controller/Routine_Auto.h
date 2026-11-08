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
    /// <para>Mỗi lượt là MỘT HÀNG tray: Y đưa hàng tới, Z hạ, 7 van bật, Z nâng (S20) → từng nozzle đang giữ can tới điểm
    /// Laser(k) + Trim, DO kích laser giữ MarkTimeMs (S30) → từng can vừa khắc tới trước CAM, Flow kiểm tra trả Result của
    /// Group: 1 = OK, khác = NG (S40) → trả cả hàng về đúng ô (S50). Hết hàng của cả hai tray thì về vị trí chờ, chờ thay
    /// tray và bấm START (S60). Không bù độ lệch can — vision chỉ kiểm tra sau khi khắc (Tuấn 2026-11-08). Hút hụt → ô NG,
    /// không khắc; kiểm tra NG hoặc không kiểm tra được → ô NG; can vẫn trả về ô.</para>
    ///
    /// <para><b>Chạy lẻ:</b> có ô chọn (<see cref="RunningData::HasCellSelection"/>) thì chỉ các ô đó mới hút, khắc, kiểm tra và thổi
    /// nhả, vẫn mỗi lượt một hàng; xong về vị trí chờ và kết thúc chu trình (máy về Idle). Ô không chọn vẫn là chưa chạy, lần
    /// START sau chạy tiếp được, ô đã xong không chạy lại — không coi tray là xong như ShieldCanAssy, vì START lúc chờ thay
    /// tray đặt lại cả tray và can đã khắc sẽ bị khắc đè. Z chung: nozzle ngoài phạm vi vẫn hạ theo, chỉ không bật van.</para>
    ///
    /// <para><b>Checkpoint</b> = sau khi trả hàng về tray (giữa các hàng) và trước bước laser (hàng vừa gắp thì trả về trước
    /// rồi mới dừng — không bao giờ dừng với can trên nozzle; hàng chưa khắc nên lần START sau chạy lại). Đã khắc thì kiểm
    /// tra và trả hàng xong mới dừng: khắc lại là khắc đè. STOP giữa lệnh trục: giữ nguyên chỗ, START chạy tiếp (bit RUN
    /// của McFunction). Lệnh khắc không bị cắt ngang.</para>
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

        /// <summary>Dừng/chạy lại đồng hồ thời gian một hàng khi tạm dừng (STOP) và chạy tiếp.</summary>
        void HoldClock(bool hold);

        /// <summary>
        /// Chạy liên tục (HÀM CHẶN, luồng nền): hàng tiếp theo của tray còn hàng → hết tray chờ thay → lặp lại, tới khi được
        /// yêu cầu dừng hoặc chạy lẻ xong. true = kết thúc bình thường (máy về Idle), false = dừng giữa chừng (máy về Paused). Bị huỷ có thể
        /// ném <c>base::OperationCanceled</c>.
        /// </summary>
        bool Run(std::stop_token ct);

        /// <summary>Quên sơ đồ tray chu trình đã ghi — trang Main suy lại từ RunningData (RESET TRAY).</summary>
        static void ClearTrayView();

        /// <summary>Một dòng cho trang Main: số ô đã chạy của 2 tray và kết quả hàng vừa xong.</summary>
        static std::string ProgressText();

        /// <summary>
        /// Sơ đồ tray cho trang Main — ECellState từng ô, chu trình ghi trực tiếp lúc chạy. <see cref="Trays"/>: tray 1, 2,
        /// mỗi tray TrayRows × 7 ô theo hàng, ô = hàng × 7 + cột (Loaded = chưa chạy, Processing = ô đang chạy, Ok / Ng =
        /// đã xong, Skipped = ô đã xong từ phiên trước; mặc định mọi ô Loaded, kể cả tray vắng). Mảng rỗng / sai kích thước = chu trình chưa
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
            /// <summary>ECellState: Empty = không giữ can, Loaded = đang giữ, Processing = đang khắc/kiểm tra, Ok = kiểm tra
            /// đạt (hoặc đã khắc khi tắt vision), Ng.</summary>
            int State{ 0 };
            /// <summary>Output Result của lần kiểm tra gần nhất (có khi đọc được).</summary>
            bool HasResult{ false };
            double Result{ 0 };
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
            std::array<bool, NozzleCount + 1> Active{};   // ô trong phạm vi lượt này: chưa xong, và đang chọn ô thì phải là ô chọn
            std::array<bool, NozzleCount + 1> Holding{};
            std::array<int, NozzleCount + 1> Result{};    // ECellState của từng ô
            std::array<bool, NozzleCount + 1> Marked{};   // đã khắc (hoặc đã đi qua điểm khắc khi chạy khô)
        };

        bool PrepareStart(std::stop_token ct);
        bool NextRow(int& tray, int& row) const;
        bool WaitTrays(std::stop_token ct);
        bool FinishSelection(std::stop_token ct);
        bool ToStandby(std::stop_token ct);
        void FinishCycle();
        bool RunRow(int tray, int row, std::stop_token ct);
        bool PickRow(Row& r, std::stop_token ct);
        bool Laser(Row& r, std::stop_token ct);
        bool Inspect(Row& r, std::stop_token ct);
        bool PlaceRow(Row& r, std::stop_token ct);
        void Finish(const Row& r);

        /// <summary>Kiểm tra can trên nozzle k tại chỗ (đã ở điểm CAM): chạy Flow, đọc Result của Group.</summary>
        VmInspectRead InspectHere(int nozzle, std::stop_token ct);
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
        std::shared_ptr<ModelData> m_model;   // bản model chốt lúc bắt đầu Run
        std::string m_lastRowText;            // "Row done: …" — giữ ở đầu dòng nhắc lúc chờ
    };
}
