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
    /// <summary>Kết quả một nút hiệu chuẩn: đạt hay không, và MỘT dòng để trang Vision Edit hiện (cả khi không đạt).</summary>
    struct CalibResult
    {
        bool Ok{ false };
        /// <summary>Dòng kết quả — số liệu khi đạt, lý do khi không đạt.</summary>
        std::string Text;

        static CalibResult Done(std::string text) { CalibResult r; r.Ok = true; r.Text = std::move(text); return r; }
        static CalibResult Fail(std::string why) { CalibResult r; r.Text = std::move(why); return r; }
    };

    /// <summary>
    /// Các bước hiệu chuẩn vision của trang Vision Edit — máy Laser Marking (PLAN-LASER.md §4.2):
    /// <list type="number">
    /// <item><b>Lưới 9 điểm X × Z</b> — nozzle <c>CalibNozzle</c> ngậm can mẫu, nạp điểm cho N-Point Calibration của Flow
    /// LocateCan (nhánh hiệu chuẩn — biến rẽ nhánh = 0).</item>
    /// <item><b>Kiểm tra lưới</b> — chạy X, Z một quãng đã biết, can dính trên nozzle thì Rel = Obj − encoder phải đứng yên.</item>
    /// <item><b>SET MASTER</b> từng nozzle — tới điểm CAM(k), trung bình N ảnh, ghi Ref[k] vào model.</item>
    /// <item><b>Thử laser</b> từng nozzle — tới điểm Laser(k) + bù theo lần soi gần nhất (không kích), TEST MARK, trim.</item>
    /// </list>
    ///
    /// <para>Mọi hàm (trừ getter) là HÀM CHẶN, gọi từ luồng nền, MỘT việc một lúc (trang Vision Edit khoá nút khi bận).
    /// Trạng thái giữa các nút nằm ở đây (không ở trang), để bài tự kiểm gọi thẳng được.</para>
    /// </summary>
    class Routine_Calib
    {
    public:
        static constexpr int NozzleCount = VisionCalib::NozzleCount;

        /// <summary>Thứ tự 9 điểm "Chạy 9 điểm" theo số numpad — hình chữ S, ít đảo chiều nhất.</summary>
        static constexpr std::array<int, 9> GridSOrder{ 1, 2, 3, 6, 5, 4, 7, 8, 9 };

        Routine_Calib(MotionConnect& motion, McFunction& func, std::function<std::shared_ptr<SystemData>()> sys,
                      std::function<std::shared_ptr<IVisionDevice>()> vision);

        // ── Lưới ──
        /// <summary>Lưu vị trí X, Z hiện tại làm tâm lưới (điểm 5) và ghi system.json. Không chạy trục.</summary>
        CalibResult SaveGridCenter();
        /// <summary>Chạy tới điểm numpad 1…9 của lưới rồi kích LocateCan nhánh hiệu chuẩn — NẠP một điểm (không xoá điểm cũ).</summary>
        CalibResult RunGridPoint(int number, std::stop_token ct);
        /// <summary>Tự bấm Clear Point của module N-point rồi chạy cả 9 điểm theo <see cref="GridSOrder"/>, dừng ngay khi
        /// một điểm thất bại. Không xoá được thì dừng, chưa chạy trục nào.</summary>
        CalibResult RunGrid(const std::function<void(const std::string&)>& progress, std::stop_token ct);
        /// <summary>Kích LocateCan nhánh hiệu chuẩn tại chỗ — cũng NẠP một điểm.</summary>
        CalibResult TriggerHere(std::stop_token ct);
        /// <summary>Kiểm tra lưới: soi tại chỗ, chạy X +quãng, soi, về; chạy Z +quãng, soi, về. Đạt thì ghi model.</summary>
        CalibResult VerifyGrid(std::stop_token ct);

        // ── Master từng nozzle ──
        /// <summary>Đưa nozzle tới giữa khung CAM (điểm CAM(k)).</summary>
        CalibResult GoToCam(int nozzle, std::stop_token ct);
        /// <summary>Soi một ảnh tại chỗ, cất Rel và độ lệch so với Ref (nếu có) để trang hiện và để GoToLaser dùng.</summary>
        CalibResult Capture(int nozzle, std::stop_token ct);
        /// <summary>Tới điểm CAM(k), trung bình N ảnh với can mẫu lắp chuẩn, ghi Ref[k] vào model.</summary>
        CalibResult SetMaster(int nozzle, std::stop_token ct);

        // ── Thử laser ──
        /// <summary>Tới điểm Laser(k) + Trim + bù theo lần <see cref="Capture"/> gần nhất của nozzle đó (chưa soi thì không bù).
        /// Không kích laser.</summary>
        CalibResult GoToLaser(int nozzle, std::stop_token ct);
        /// <summary>Kích DO laser tại chỗ đang đứng (MarkTimeMs của model).</summary>
        CalibResult TestMark(std::stop_token ct);
        /// <summary>Lần soi gần nhất của nozzle: Rel và độ lệch; nullopt = chưa soi.</summary>
        struct LastRead { double RelX, RelZ, Dx, Dz; };
        std::optional<LastRead> Last(int nozzle) const;

        /// <summary>Ghi Trim (mm) của nozzle vào model và lưu. Rỗng = đã ghi; khác rỗng = lý do không ghi.</summary>
        std::string SetTrim(int nozzle, double x, double z);

        // ── Cổng ──
        VmCalibGate CheckCalib(int nozzle = 0) const;

        /// <summary>Bỏ mọi trạng thái giữa các nút — bài tự kiểm dựng lại trạng thái đầu.</summary>
        void Reset();

    private:
        CalibResult Guard(const char* what, const std::function<CalibResult()>& body);
        VmMeanCapture CaptureMean(int nozzle, int shots, std::stop_token ct);

        /// <summary>Nâng Z an toàn, chạy các trục ngang song song, rồi hạ Z tới <paramref name="z"/> nếu có.</summary>
        bool MoveTo(const std::vector<std::pair<std::string, double>>& axes, std::optional<double> z, std::stop_token ct);
        bool MoveOne(const std::string& axis, double pos, std::stop_token ct);
        bool Holding(int nozzle) const;
        void Lamp(bool on);
        VmCallResult Call(const VmCallRequest& req, std::stop_token ct);
        VisionPose Trigger(int nozzle, std::stop_token ct);
        /// <summary>Clear Point trước một chuỗi nạp điểm — rỗng = đã xoá, khác rỗng = lý do không xoá được.</summary>
        std::string ClearPoints();

        std::shared_ptr<TeachPosition> CamPoint(int nozzle, std::string& why) const;
        std::shared_ptr<TeachPosition> LaserPoint(int nozzle, std::string& why) const;
        double Pos(const char* axis) const;
        std::string SaveModelNote() const;
        void Log(const std::string& msg, bool warn = false) const;

        MotionConnect& m_motion;
        McFunction& m_func;
        std::function<std::shared_ptr<SystemData>()> m_sys;
        std::function<std::shared_ptr<IVisionDevice>()> m_vision;

        mutable std::mutex m_mutex;
        std::array<std::optional<LastRead>, NozzleCount + 1> m_last{};
    };
}
