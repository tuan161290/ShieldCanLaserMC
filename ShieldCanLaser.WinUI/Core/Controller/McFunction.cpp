#include "Controller/McFunction.h"

#include <climits>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

#include "Infor/AxisId.h"
#include "Utility/Log/LogWorker.h"

namespace scs
{
    namespace
    {
        const char* const HeldReason = "machine paused — press START to resume, or switch to MANUAL to abort cycle";

        thread_local bool t_inCycle = false;

        template <class Arr>
        bool Contains(const Arr& names, std::string_view name)
        {
            for (const char* n : names) if (name == n) return true;
            return false;
        }
    }

    const long long McFunction::NeverMoved = LLONG_MIN;

    McFunction::McFunction(MotionConnect& motion, SafetyMonitor& safety, McStatus& status, std::function<std::shared_ptr<SystemData>()> sys)
        : m_motion(motion), m_safety(safety), m_status(status), m_sys(std::move(sys)), m_lastMoveEndedMs(NeverMoved)
    {
    }

    void McFunction::MarkCycleFlow() { t_inCycle = true; }
    bool McFunction::InCycleFlow() { return t_inCycle; }
    McFunction::CycleFlowScope::CycleFlowScope(bool inCycle) : m_before(t_inCycle) { t_inCycle = inCycle; }
    McFunction::CycleFlowScope::~CycleFlowScope() { t_inCycle = m_before; }

    bool McFunction::CanMove(std::string_view axisName, std::string& reason) const
    {
        reason.clear();
        std::string name(axisName);

        auto axis = m_motion.Axis(axisName);
        if (!axis) { reason = "no axis " + name; return false; }

        if (!m_safety.SafeToMove())
        {
            reason = m_safety.BlockReason();
            return false;
        }
        if (!axis->IsConnected()) { reason = name + " disconnected"; return false; }
        if (axis->Alarm())        { reason = name + " in alarm"; return false; }
        if (!axis->ServoOn())     { reason = name + " servo OFF"; return false; }

        // ── Chốt chặn chống va ──
        // X, Y chỉ được chạy khi Z ở cao độ an toàn. Kiểm tra bằng
        // vị trí thực đọc từ encoder, không phải bằng việc lệnh nâng Z trước đó đã trả về thành công.
        if (Contains(AxisId::PlanarAxes, axisName) && !m_motion.ZSafeOk())
        {
            reason = "Z not at safe height: " + m_motion.ZUnsafeText();
            return false;
        }

        return true;
    }

    bool McFunction::CanMoveAutonomous(std::string_view axisName, std::string& reason) const
    {
        if (!CanMove(axisName, reason)) return false;
        if (!m_runBit) { reason = HeldReason; return false; }

        if (m_motion.ZSafeInterlock() && Contains(AxisId::PlanarAxes, axisName))
        {
            for (const char* zName : AxisId::ZAxes)
            {
                auto z = m_motion.Axis(zName);
                if (!z || !z->OrgOk())
                {
                    reason = std::string(zName) + " must be homed before " + std::string(axisName) + " Home/Move Abs";
                    return false;
                }
            }
        }

        return true;
    }

    bool McFunction::Move(const std::string& axisName, double pos, double speed, std::stop_token ct)
    {
        return MoveHoldable(axisName, pos, speed, ct, true);
    }

    bool McFunction::MoveGentle(const std::string& axisName, double pos, double speed, std::stop_token ct)
    {
        return MoveHoldable(axisName, pos, speed, ct, true, true);
    }

    void McFunction::SetRunBit(bool on)
    {
        std::stop_source cancel;
        bool doCancel = false;
        {
            std::lock_guard<std::mutex> lock(m_runLock);
            if (m_runBit == on) return;
            m_runBit = on;
            if (on) m_runCts = std::stop_source();
            else { cancel = m_runCts; doCancel = true; }
        }
        if (doCancel) cancel.request_stop();
        if (!on) m_motion.StopAll();
    }

    void McFunction::WaitRunBit(std::stop_token ct)
    {
        while (!m_runBit) base::Delay(20, ct);
    }

    /// Một lệnh chạy tuyệt đối theo bit RUN: bit tắt thì chờ, bị tắt giữa hành trình thì chờ bật lại rồi
    /// phát lại đúng lệnh cũ. interlock = false chỉ cho lệnh nâng Z của EnsureZSafe.
    bool McFunction::MoveHoldable(const std::string& axisName, double pos, double speed, std::stop_token ct, bool interlock, bool gentle)
    {
        if (!m_runBit && !t_inCycle)
        {
            m_status.SetMessage("Cannot move " + axisName + ": " + HeldReason);
            return false;
        }

        auto axis = m_motion.Axis(axisName);
        if (!axis) { m_status.SetMessage("No axis " + axisName); return false; }

        while (true)
        {
            WaitRunBit(ct);

            std::string reason;
            if (interlock && !CanMove(axisName, reason))
            {
                // Rèm sáng vừa bị cắt giữa lúc qua chờ bit và lúc xét chốt: bit RUN đã tắt trước khi
                // SafeToMove tắt, nên quay lại chờ START như STOP thay vì báo lỗi dừng chu trình.
                if (!m_runBit && t_inCycle) continue;
                std::string msg = "Cannot move " + axisName + ": " + reason;
                m_status.SetMessage(msg);
                LogWorker::Instance().RecordLog(msg, LogLevel::Warning, axisName);
                return false;
            }

            std::stop_source run;
            {
                std::lock_guard<std::mutex> lock(m_runLock);
                run = m_runCts;
            }
            bool ok;
            try
            {
                base::LinkedStopSource linked(ct, run.get_token());
                ok = axis->MoveAbs(pos, speed, linked.Token(), gentle);
            }
            catch (const base::OperationCanceled&)
            {
                if (!(!ct.stop_requested() && run.stop_requested())) throw;
                ok = false;
            }
            MarkMoveEnded();

            if (ok) return true;
            if (ct.stop_requested() || !run.stop_requested()) return false;
            // Bit RUN tắt giữa hành trình: quay lại chờ bật rồi phát lại lệnh.
        }
    }

    int McFunction::SettleRemainingMs(long long lastMoveEndedMs, long long nowMs, int settleMs)
    {
        if (settleMs <= 0 || lastMoveEndedMs == NeverMoved) return 0;
        long long left = settleMs - (nowMs - lastMoveEndedMs);
        if (left <= 0) return 0;
        return left > settleMs ? settleMs : (int)left;
    }

    void McFunction::WaitSettle(std::stop_token ct)
    {
        int left = SettleRemainingMs(m_lastMoveEndedMs.load(), m_clock.ElapsedMilliseconds(), m_sys()->VisionSettleMs);
        if (left > 0) base::Delay(left, ct);
    }

    bool McFunction::MoveZDown(const std::string& zAxis, double target, double slowAt, std::stop_token ct)
    {
        auto cfg = m_sys()->GetAxis(zAxis);
        auto axis = m_motion.Axis(zAxis);
        double now = axis ? axis->CurrentPos() : std::numeric_limits<double>::quiet_NaN();

        if (!UseTwoStageZ(now, slowAt, target))
            return Move(zAxis, target, cfg->MoveSpeed, ct);

        if (!Move(zAxis, slowAt, cfg->MoveSpeed, ct)) return false;
        return Move(zAxis, target, cfg->MoveSpeedLow(), ct);
    }

    bool McFunction::MoveZUp(const std::string& zAxis, double target, double slowAt, std::stop_token ct)
    {
        auto cfg = m_sys()->GetAxis(zAxis);
        auto axis = m_motion.Axis(zAxis);
        double now = axis ? axis->CurrentPos() : std::numeric_limits<double>::quiet_NaN();

        if (!UseTwoStageZUp(now, slowAt, target))
            return Move(zAxis, target, cfg->MoveSpeed, ct);

        if (!Move(zAxis, slowAt, cfg->MoveSpeedLow(), ct)) return false;
        return Move(zAxis, target, cfg->MoveSpeed, ct);
    }

    bool McFunction::MoveInc(const std::string& axisName, double delta, double speed, std::stop_token ct)
    {
        std::string reason = m_runBit ? std::string() : std::string(HeldReason);
        if (!reason.empty() || !CanMove(axisName, reason))
        {
            std::string msg = "Cannot move " + axisName + ": " + reason;
            m_status.SetMessage(msg);
            LogWorker::Instance().RecordLog(msg, LogLevel::Warning, axisName);
            return false;
        }

        auto axis = m_motion.Axis(axisName);
        bool ok = axis->MoveInc(delta, speed, ct);
        MarkMoveEnded();
        return ok;
    }

    void McFunction::Jog(const std::string& axisName, int dir, double speed)
    {
        std::string reason;
        if (dir != 0 && !m_runBit)
        {
            m_status.SetMessage("Cannot jog " + axisName + ": " + HeldReason);
            return;
        }
        if (dir != 0 && !CanMove(axisName, reason))
        {
            m_status.SetMessage("Cannot jog " + axisName + ": " + reason);
            return;
        }

        auto axis = m_motion.Axis(axisName);
        if (!axis) return;

        auto cfg = axis->Config();
        double v = speed > 0 ? speed : (m_jogFast ? cfg->JogSpeedFast : cfg->JogSpeedSlow);
        axis->Jog(dir, v);
    }

    bool McFunction::EnsureZSafe(std::stop_token ct)
    {
        if (m_motion.ZSafeOk()) return true;

        // "An toàn" là vị trí NHỎ (đã rút lên). Dừng đúng cao độ an toàn Z_SAFE:
        // chốt chặn đã nới thêm một dung sai.
        bool inCycle = t_inCycle;
        std::vector<std::function<bool()>> jobs;
        for (const char* n : AxisId::ZAxes)
        {
            auto a = m_motion.Axis(n);
            double target = m_motion.ZSafeFor(n);
            if (!a || !(a->CurrentPos() > target)) continue;
            jobs.push_back([this, a, target, ct, inCycle]
            {
                CycleFlowScope scope(inCycle);
                return MoveHoldable(a->Name(), target, a->Config()->MoveSpeed, ct, false);
            });
        }

        if (jobs.empty()) return m_motion.ZSafeOk();

        bool all = base::WhenAll(std::move(jobs));
        return all && m_motion.ZSafeOk();
    }

    bool McFunction::MoveToPosition(const TeachPosition* point, std::stop_token ct, bool slow)
    {
        if (point == nullptr || point->Pos.empty())
        {
            m_status.SetMessage("Teach point has no coordinates");
            return false;
        }

        if (!EnsureZSafe(ct))
        {
            m_status.SetMessage("Cannot raise Z to safe height");
            return false;
        }

        auto sys = m_sys();
        bool inCycle = t_inCycle;
        std::vector<std::function<bool()>> jobs;
        for (const auto& kv : point->Pos)
        {
            if (!AxisId::IsValid(kv.first) || Contains(AxisId::ZAxes, kv.first)) continue;
            std::string name = kv.first;
            double pos = kv.second;
            auto cfg = sys->GetAxis(name);
            double speed = slow ? cfg->MoveSpeedLow() : cfg->MoveSpeed;
            jobs.push_back([this, name, pos, speed, ct, inCycle]
            {
                CycleFlowScope scope(inCycle);
                return Move(name, pos, speed, ct);
            });
        }
        if (!jobs.empty() && !base::WhenAll(std::move(jobs))) return false;

        for (const char* z : AxisId::ZAxes)
        {
            if (!point->Has(z)) continue;
            auto cfg = sys->GetAxis(z);
            if (!Move(z, point->Get(z), slow ? cfg->MoveSpeedLow() : cfg->MoveSpeed, ct))
                return false;
        }
        return true;
    }
}
