#include "Controller/Routine_Origin.h"

#include <cmath>
#include <format>

#include "Base/Async.h"
#include "Communication/AxisAngle.h"
#include "Infor/AlarmManager.h"
#include "Infor/AxisId.h"
#include "Utility/Log/LogWorker.h"

namespace scs
{
    namespace
    {
        template <class Arr>
        std::vector<std::string> Names(const Arr& a) { return std::vector<std::string>(a.begin(), a.end()); }

        template <class Arr>
        bool Contains(const Arr& names, std::string_view name)
        {
            for (const char* n : names) if (name == n) return true;
            return false;
        }

        void Log(const std::string& msg, LogLevel level) { LogWorker::Instance().RecordLog(msg, level, "Origin"); }
    }

    Routine_Origin::Routine_Origin(MotionConnect& motion, McFunction& func, McStatus& status, std::function<std::shared_ptr<SystemData>()> sys)
        : m_motion(motion), m_func(func), m_status(status), m_sys(std::move(sys))
    {
    }

    bool Routine_Origin::Run(std::stop_token ct)
    {
        auto sys = m_sys();
        m_status.SetOrgOk(false);

        try
        {
            // KHÔNG gọi Abort() ở đây — Abort() tự đặt Message bằng một câu chung chung, đè
            // mất lý do cụ thể mà Fail() đã ghi bên trong PreCheck. Tự làm phần còn lại của
            // Abort() mà không đụng vào Message.
            if (!PreCheck())
            {
                m_motion.StopAll();
                m_status.SetOrgOk(false);
                m_status.SetOriginStep(EOriginState::Aborted);
                return false;
            }

            Step(EOriginState::O1_ServoOn);
            if (!ServoOnAll(ct)) return Abort("servo ON failed");

            Step(EOriginState::O2_HomeZ);
            if (!HomeParallel(Names(AxisId::ZAxes), ct)) return Abort("Z home failed");

            Step(EOriginState::O3_GateZSafe);
            if (!GateZSafe()) return Abort("Z not at safe height");

            // X và Y về gốc song song — Z đã qua chốt O3.
            (void)sys;
            Step(EOriginState::O5_HomeXY);
            if (!HomeParallel(Names(AxisId::PlanarAxes), ct)) return Abort("X/Y home failed");

            Step(EOriginState::O6_Standby);
            if (!MoveStandby(ct)) return Abort("move to standby failed");

            Step(EOriginState::O7_Finish);
            bool homed = m_motion.AllHomed();
            m_status.SetOrgOk(homed);
            Log(homed ? "Origin complete" : "Origin finished but some axes not homed", homed ? LogLevel::Info : LogLevel::Warning);
            return homed;
        }
        catch (const base::OperationCanceled&)
        {
            return Abort("cancelled");
        }
        catch (const std::exception& ex)
        {
            AlarmManager::RaiseEvent(AlarmCode::RoutineCrashed, "Origin", ex.what());   // sự kiện: RESET gỡ được (30/09)
            return Abort(std::string("unexpected error: ") + ex.what());
        }
    }

    bool Routine_Origin::Init(std::stop_token ct)
    {
        try
        {
            if (!m_motion.AllHomed()) { m_status.SetMessage("Cannot Init: not homed — press Origin"); return false; }
            if (!m_func.EnsureZSafe(ct))
            {
                m_status.SetMessage("Cannot Init: cannot raise Z to safe height");
                return false;
            }
            if (!MoveStandby(ct))
            {
                m_status.SetMessage("Cannot Init: axis failed to reach standby");
                return false;
            }
            Log("Init done — all axes at standby", LogLevel::Info);
            return true;
        }
        catch (const base::OperationCanceled&)
        {
            m_status.SetMessage("Init cancelled");
            return false;
        }
        catch (const std::exception& ex)
        {
            // Không bắt thì luồng lỗi, đoạn chạy sau Init (McControl::DoInit) không chạy và máy kẹt ở trạng thái về gốc.
            AlarmManager::RaiseEvent(AlarmCode::RoutineCrashed, "Origin", ex.what());
            m_status.SetMessage(std::string("Init unexpected error: ") + ex.what());
            return false;
        }
    }

    // ─────────────── O0 ───────────────

    /// Toàn bộ danh sách phải PASS mới đi tiếp — PLAN.md §4.2.
    bool Routine_Origin::PreCheck()
    {
        Step(EOriginState::O0_PreCheck);

        if (!m_motion.AllConnected()) return Fail("axis not connected");
        if (m_motion.AnyAlarm()) return Fail("axis alarm — press RESET first");
        if (!m_status.EmgOk()) return Fail("EMG active");
        if (!m_status.LightCurtainOk()) return Fail("photo safety interrupted");

        // Phanh trục Z: driver servo EtherCAT tự nhả khi Operation Enabled và tự khoá khi Servo OFF/lỗi
        // (khác máy cũ, nơi App điều khiển phanh Z1/Z2 qua DO) — không có điều kiện phanh ở đây.

        if (m_status.State() == EMcState::Running) return Fail("machine running Auto");

        return true;
    }

    bool Routine_Origin::Fail(const std::string& reason)
    {
        std::string msg = "Cannot home: " + reason;
        m_status.SetMessage(msg);
        Log(msg, LogLevel::Warning);
        return false;
    }

    // ─────────────── O1 ───────────────

    bool Routine_Origin::ServoOnAll(std::stop_token ct)
    {
        if (ct.stop_requested()) return false;

        // Song song: servo EtherCAT có thể mất 1–2 s mới vào Operation Enabled (nhả phanh). Trục đã bật sẵn thì bỏ qua.
        std::vector<std::shared_ptr<IAxis>> pending;
        for (const auto& a : m_motion.AxisList()) if (!a->ServoOn()) pending.push_back(a);
        std::vector<char> results(pending.size(), 0);
        std::vector<std::function<bool()>> jobs;
        for (size_t i = 0; i < pending.size(); ++i)
            jobs.push_back([&pending, &results, i] { results[i] = pending[i]->SetServoOn(true) ? 1 : 0; return true; });
        if (!jobs.empty()) base::WhenAll(std::move(jobs));
        for (size_t i = 0; i < pending.size(); ++i)
        {
            if (results[i]) continue;
            AlarmManager::RaiseAxisEvent(pending[i]->Name(), AxisFault::ServoOffUnexpected, "cannot turn servo ON");
            return false;
        }

        // Chờ driver báo sẵn sàng thay vì giả định servo on là dùng được ngay.
        base::Stopwatch sw;
        while (!m_motion.AllServoOn())
        {
            if (ct.stop_requested()) return false;
            if (sw.ElapsedMilliseconds() > 5000) return false;
            base::Delay(20);
        }
        return true;
    }

    // ─────────────── O2 / O4 / O5 ───────────────

    bool Routine_Origin::HomeParallel(const std::vector<std::string>& names, std::stop_token ct)
    {
        std::vector<std::shared_ptr<IAxis>> axes;
        std::string joined;
        for (const auto& n : names)
        {
            if (!joined.empty()) joined += ", ";
            joined += n;
            if (auto a = m_motion.Axis(n)) axes.push_back(a);
        }
        if (axes.empty()) return true;

        Log("Parallel home: " + joined, LogLevel::Info);

        // Một nguồn huỷ liên kết: trục nào hỏng thì huỷ luôn các trục còn lại,
        // thay vì để chúng chạy tiếp trong khi máy đã ở trạng thái không xác định.
        base::LinkedStopSource linked(ct);
        std::vector<std::function<bool()>> jobs;
        for (const auto& a : axes)
            jobs.push_back([this, a, &linked]
            {
                bool ok = m_motion.Home(a, linked.Token());
                if (!ok) linked.Cancel();
                return ok;
            });
        return base::WhenAll(std::move(jobs));
    }

    bool Routine_Origin::HomeSequential(const std::vector<std::string>& names, std::stop_token ct)
    {
        for (const auto& name : names)
        {
            auto axis = m_motion.Axis(name);
            if (!axis) continue;
            if (!m_motion.Home(axis, ct)) return false;
        }
        return true;
    }

    // ─────────────── O3 ───────────────

    /// Chốt chặn: đọc LẠI vị trí thực và cờ về gốc của trục Z. Không đạt thì huỷ toàn
    /// bộ, không đi tiếp. Đây là chỗ duy nhất ngăn X/Y chạy ngang khi một trục Z còn thấp.
    bool Routine_Origin::GateZSafe()
    {
        for (const char* name : AxisId::ZAxes)
        {
            auto z = m_motion.Axis(name);
            if (!z) return false;
            if (!z->OrgOk())
            {
                m_status.SetMessage(std::format("Z interlock: axis {} not homed", name));
                return false;
            }
        }
        if (!m_motion.ZSafeOk())
        {
            m_status.SetMessage("Z interlock: " + m_motion.ZUnsafeText());
            return false;
        }

        auto z = m_motion.Axis(AxisId::Z);
        Log(std::format("Z interlock OK: Z={:.2f} ≤ {:.2f}", z->CurrentPos(), m_motion.ZSafeLimit()), LogLevel::Info);
        return true;
    }

    // ─────────────── O6 ───────────────

    /// Mọi trục về vị trí chờ: các trục không phải Z chạy song song (Z đang ở cao độ an toàn), Z đi
    /// SAU CÙNG — vị trí chờ của Z có thể nằm dưới Z_SAFE, đi trước thì X/Y bị chốt chặn giữ lại.
    bool Routine_Origin::MoveStandby(std::stop_token ct)
    {
        std::vector<std::function<bool()>> jobs;
        for (const auto& a : m_motion.AxisList())
        {
            if (Contains(AxisId::ZAxes, a->Name())) continue;
            jobs.push_back([this, a, ct] { return MoveStandbyOne(a, ct); });
        }
        if (!jobs.empty() && !base::WhenAll(std::move(jobs))) return false;

        for (const char* n : AxisId::ZAxes)
        {
            auto z = m_motion.Axis(n);
            if (!z) continue;
            if (!MoveStandbyOne(z, ct)) return false;
        }
        return true;
    }

    bool Routine_Origin::MoveStandbyOne(const std::shared_ptr<IAxis>& axis, std::stop_token ct)
    {
        if (ct.stop_requested()) return false;
        auto cfg = axis->Config();
        bool there = cfg->WrapDegrees
            ? AxisAngle::Distance(axis->CurrentPos(), cfg->StandbyPos) < 0.01
            : std::fabs(axis->CurrentPos() - cfg->StandbyPos) < 0.01;
        if (there) return true;
        return m_func.Move(axis->Name(), cfg->StandbyPos, cfg->MoveSpeed, ct);
    }

    // ─────────────── tiện ích ───────────────

    void Routine_Origin::Step(EOriginState step)
    {
        m_status.SetOriginStep(step);
        Log(std::string("Origin ") + ToString(step), LogLevel::Info);
    }

    /// Huỷ giữa chừng — PLAN.md §4.4. Không bao giờ để trạng thái "về gốc dở" được đánh dấu
    /// là đã về gốc: OrgOk phải về false và ghi rõ trục nào ở bước nào gây huỷ.
    bool Routine_Origin::Abort(const std::string& reason)
    {
        m_motion.StopAll();
        m_status.SetOrgOk(false);
        std::string msg = std::string("Origin aborted at step ") + ToString(m_status.OriginStep()) + ": " + reason;
        m_status.SetMessage(msg);
        m_status.SetOriginStep(EOriginState::Aborted);
        Log(msg, LogLevel::Error);
        return false;
    }
}
