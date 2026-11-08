#pragma once
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "Communication/IVisionDevice.h"
#include "Communication/MotionConnect.h"
#include "Controller/McFunction.h"
#include "Controller/VmCycle.h"
#include "Data/Model/ModelData.h"
#include "Data/SystemData.h"

namespace scs
{
    /// <summary>Kết quả một nút của trang Vision Edit: đạt hay không, và MỘT dòng để trang hiện (cả khi không đạt).</summary>
    struct CalibResult
    {
        bool Ok{ false };
        /// <summary>Dòng kết quả — số liệu khi đạt, lý do khi không đạt.</summary>
        std::string Text;

        static CalibResult Done(std::string text) { CalibResult r; r.Ok = true; r.Text = std::move(text); return r; }
        static CalibResult Fail(std::string why) { CalibResult r; r.Text = std::move(why); return r; }
    };

    /// <summary>
    /// Các nút thử của trang Vision Edit — máy Laser Marking (PLAN-LASER.md §4). Vision chỉ kiểm tra sau khi khắc, không
    /// có hiệu chuẩn camera: ở đây chỉ còn thử khắc từng nozzle để chỉnh <b>Trim</b> của điểm khắc, và thử kiểm tra một can
    /// trên nozzle để xem Flow trả Result gì.
    ///
    /// <para>Mọi hàm (trừ getter) là HÀM CHẶN, gọi từ luồng nền, MỘT việc một lúc (trang Vision Edit khoá nút khi bận).</para>
    /// </summary>
    class Routine_Calib
    {
    public:
        static constexpr int NozzleCount = LaserTrim::NozzleCount;

        Routine_Calib(MotionConnect& motion, McFunction& func, std::function<std::shared_ptr<SystemData>()> sys,
                      std::function<std::shared_ptr<IVisionDevice>()> vision);

        // ── Thử khắc ──
        /// <summary>Tới điểm Laser(k) + Trim của nozzle. Không kích laser.</summary>
        CalibResult GoToLaser(int nozzle, std::stop_token ct);
        /// <summary>Kích DO laser tại chỗ đang đứng (MarkTimeMs của model).</summary>
        CalibResult TestMark(std::stop_token ct);
        /// <summary>Ghi Trim (mm) của nozzle vào model và lưu. Rỗng = đã ghi; khác rỗng = lý do không ghi.</summary>
        std::string SetTrim(int nozzle, double x, double z);

        // ── Thử kiểm tra ──
        /// <summary>Đưa nozzle tới giữa khung CAM (điểm CAM(k)).</summary>
        CalibResult GoToCam(int nozzle, std::stop_token ct);
        /// <summary>Kiểm tra can trên nozzle tại chỗ: chạy Flow, đọc Result của Group (đèn CAM bật trong lúc chụp).</summary>
        CalibResult Inspect(int nozzle, std::stop_token ct);
        /// <summary>Lần kiểm tra gần nhất của nozzle; nullopt = chưa kiểm tra.</summary>
        std::optional<VmInspectRead> LastInspect(int nozzle) const;

        /// <summary>Bỏ mọi trạng thái giữa các nút — bài tự kiểm dựng lại trạng thái đầu.</summary>
        void Reset();

    private:
        CalibResult Guard(const char* what, const std::function<CalibResult()>& body);

        /// <summary>Nâng Z an toàn, chạy các trục ngang song song, rồi hạ Z tới <paramref name="z"/> nếu có.</summary>
        bool MoveTo(const std::vector<std::pair<std::string, double>>& axes, std::optional<double> z, std::stop_token ct);
        bool MoveOne(const std::string& axis, double pos, std::stop_token ct);
        bool Holding(int nozzle) const;
        void Lamp(bool on);

        std::shared_ptr<TeachPosition> CamPoint(int nozzle, std::string& why) const;
        std::shared_ptr<TeachPosition> LaserPoint(int nozzle, std::string& why) const;
        double Pos(const char* axis) const;
        void Log(const std::string& msg, bool warn = false) const;

        MotionConnect& m_motion;
        McFunction& m_func;
        std::function<std::shared_ptr<SystemData>()> m_sys;
        std::function<std::shared_ptr<IVisionDevice>()> m_vision;

        mutable std::mutex m_mutex;
        std::array<std::optional<VmInspectRead>, NozzleCount + 1> m_last{};
    };
}
