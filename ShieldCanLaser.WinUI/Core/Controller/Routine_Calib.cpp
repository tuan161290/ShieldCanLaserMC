#include "Controller/Routine_Calib.h"

#include <cmath>
#include <format>

#include "Base/Async.h"
#include "Data/McDataManager.h"
#include "Infor/AlarmManager.h"
#include "Infor/AxisId.h"
#include "Infor/IoMap.h"
#include "Utility/Log/LogWorker.h"

namespace scs
{
    namespace
    {
        bool ValidNozzle(int k) { return k >= 1 && k <= Routine_Calib::NozzleCount; }
    }

    Routine_Calib::Routine_Calib(MotionConnect& motion, McFunction& func, std::function<std::shared_ptr<SystemData>()> sys,
                                 std::function<std::shared_ptr<IVisionDevice>()> vision)
        : m_motion(motion), m_func(func), m_sys(std::move(sys)), m_vision(std::move(vision))
    {
    }

    void Routine_Calib::Reset()
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (auto& r : m_last) r.reset();
    }

    std::optional<VmInspectRead> Routine_Calib::LastInspect(int nozzle) const
    {
        if (!ValidNozzle(nozzle)) return std::nullopt;
        std::lock_guard<std::mutex> l(m_mutex);
        return m_last[(size_t)nozzle];
    }

    // ═════════════════════════ Thử khắc ═════════════════════════

    CalibResult Routine_Calib::GoToLaser(int nozzle, std::stop_token ct)
    {
        return Guard("Go to laser", [&]() -> CalibResult {
            if (!ValidNozzle(nozzle)) return CalibResult::Fail("nozzle must be 1–7");
            std::string why;
            auto p = LaserPoint(nozzle, why);
            if (!p) return CalibResult::Fail(why);
            auto model = McDataManager::Model();
            const auto& trim = model->Trim.Nozzle(nozzle);
            const double x = p->Get(AxisId::X) + trim.X, z = p->Get(AxisId::Z) + trim.Z;
            if (!MoveTo({ { AxisId::X, x } }, z, ct)) return CalibResult::Fail("X/Z move to laser point failed");
            return CalibResult::Done(std::format("Nozzle {} at laser: X {:.3f}, Z {:.3f} (trim {:+.3f}/{:+.3f})", nozzle, x, z, trim.X, trim.Z));
        });
    }

    CalibResult Routine_Calib::TestMark(std::stop_token ct)
    {
        return Guard("Test mark", [&]() -> CalibResult {
            auto io = m_func.Io();
            if (!io) return CalibResult::Fail("Remote IO not available");
            if (m_motion.AnyBusy()) return CalibResult::Fail("axis moving — wait until stopped");
            auto model = McDataManager::Model();
            int hold = std::max(model->Auto.MarkTimeMs, m_sys()->LaserPulseMinMs);
            m_func.LaserPulse(hold, ct);
            return CalibResult::Done(std::format("Laser pulse {} ms at X {:.3f}, Z {:.3f}", hold, Pos(AxisId::X), Pos(AxisId::Z)));
        });
    }

    std::string Routine_Calib::SetTrim(int nozzle, double x, double z)
    {
        if (!ValidNozzle(nozzle)) return "nozzle must be 1–7";
        if (!VmMath::IsFinite(x) || !VmMath::IsFinite(z)) return "invalid number";
        if (std::fabs(x) > LaserTrim::MaxTrimMm || std::fabs(z) > LaserTrim::MaxTrimMm)
            return std::format("max trim ±{} mm — larger offset is a teach error, re-teach Laser N1/N7", LaserTrim::MaxTrimMm);
        auto model = McDataManager::Model();
        auto& trim = model->Trim.Nozzle(nozzle);
        trim.X = x;
        trim.Z = z;
        Log(std::format("Laser trim nozzle {}: X {:+.3f}, Z {:+.3f} mm", nozzle, x, z));
        if (!McDataManager::ModelLoaded()) return {};
        return McDataManager::SaveModel() ? std::string() : "MODEL SAVE FAILED — see alarm";
    }

    // ═════════════════════════ Thử kiểm tra ═════════════════════════

    CalibResult Routine_Calib::GoToCam(int nozzle, std::stop_token ct)
    {
        return Guard("Go to CAM", [&] {
            if (!ValidNozzle(nozzle)) return CalibResult::Fail("nozzle must be 1–7");
            std::string why;
            auto p = CamPoint(nozzle, why);
            if (!p) return CalibResult::Fail(why);
            std::optional<double> z;
            if (p->Has(AxisId::Z)) z = p->Get(AxisId::Z);
            if (!MoveTo({ { AxisId::X, p->Get(AxisId::X) } }, z, ct)) return CalibResult::Fail("failed to reach CAM point for nozzle " + std::to_string(nozzle));
            return CalibResult::Done(std::format("Nozzle {} at CAM: X {:.3f}, Z {:.3f}", nozzle, Pos(AxisId::X), Pos(AxisId::Z)));
        });
    }

    CalibResult Routine_Calib::Inspect(int nozzle, std::stop_token ct)
    {
        return Guard("Inspect", [&]() -> CalibResult {
            if (!ValidNozzle(nozzle)) return CalibResult::Fail("nozzle must be 1–7");
            if (!Holding(nozzle)) return CalibResult::Fail(std::format("nozzle {} not holding a can — vacuum a marked can onto it (Manual page)", nozzle));
            auto vision = m_vision ? m_vision() : nullptr;
            if (!vision) return CalibResult::Fail("vision device not available");
            const auto names = m_sys()->VisionOutputs;
            if (auto why = names.InspectPathProblem(); !why.empty()) return CalibResult::Fail(why);

            m_func.WaitSettle(ct);
            Lamp(true);
            struct Off { Routine_Calib* r; ~Off() { r->Lamp(false); } } off{ this };
            base::Delay(m_sys()->LampSettleMs, ct);
            auto read = VmCycle::ReadInspect(vision->CallFlow(VmCycle::InspectRequest(names, nozzle), ct));
            {
                std::lock_guard<std::mutex> l(m_mutex);
                m_last[(size_t)nozzle] = read;
            }
            std::string text = std::format("Nozzle {} · {}: {}", nozzle, names.InspectPath, read.Describe());
            return read.CallOk && read.HasResult ? CalibResult::Done(text) : CalibResult::Fail(text);
        });
    }

    // ═════════════════════════ tiện ích ═════════════════════════

    CalibResult Routine_Calib::Guard(const char* what, const std::function<CalibResult()>& body)
    {
        CalibResult r;
        try
        {
            r = body();
        }
        catch (const base::OperationCanceled&)
        {
            r = CalibResult::Fail("canceled");
        }
        catch (const std::exception& ex)
        {
            AlarmManager::RaiseEvent(AlarmCode::RoutineCrashed, "Calib", ex.what());
            r = CalibResult::Fail(std::string("unexpected error: ") + ex.what());
        }
        Log(std::string(what) + (r.Ok ? " — " : " stopped: ") + r.Text, !r.Ok);
        return r;
    }

    bool Routine_Calib::MoveTo(const std::vector<std::pair<std::string, double>>& axes, std::optional<double> z, std::stop_token ct)
    {
        if (!m_func.EnsureZSafe(ct)) return false;
        auto sys = m_sys();
        std::vector<std::function<bool()>> jobs;
        for (const auto& [name, pos] : axes)
        {
            std::string axis = name;
            double target = pos, speed = sys->GetAxis(name)->MoveSpeed;
            jobs.push_back([this, axis, target, speed, ct] { return m_func.Move(axis, target, speed, ct); });
        }
        if (!jobs.empty() && !base::WhenAll(std::move(jobs))) return false;
        if (z && !MoveOne(AxisId::Z, *z, ct)) return false;
        return true;
    }

    bool Routine_Calib::MoveOne(const std::string& axis, double pos, std::stop_token ct)
    {
        return m_func.Move(axis, pos, m_sys()->GetAxis(axis)->MoveSpeed, ct);
    }

    bool Routine_Calib::Holding(int nozzle) const
    {
        auto io = m_func.Io();
        return io && ValidNozzle(nozzle) && io->GetDi(IoMap::VacuumSensor(nozzle));
    }

    void Routine_Calib::Lamp(bool on)
    {
        auto io = m_func.Io();
        if (io) io->SetDo(IoMap::DoLightCam, on);
    }

    std::shared_ptr<TeachPosition> Routine_Calib::CamPoint(int nozzle, std::string& why) const
    {
        auto model = McDataManager::Model();
        if (!model) { why = "no model loaded"; return nullptr; }
        auto p = model->CamPoint(nozzle, why);
        if (p && !p->Has(AxisId::X)) { why = "point \"" + p->Name + "\" missing axis X (Teaching page)"; return nullptr; }
        return p;
    }

    std::shared_ptr<TeachPosition> Routine_Calib::LaserPoint(int nozzle, std::string& why) const
    {
        auto model = McDataManager::Model();
        if (!model) { why = "no model loaded"; return nullptr; }
        auto p = model->LaserPoint(nozzle, why);
        if (!p) return nullptr;
        for (const char* axis : { AxisId::X, AxisId::Z })
            if (!p->Has(axis)) { why = "point \"" + p->Name + "\" missing axis " + axis + " (Teaching page)"; return nullptr; }
        return p;
    }

    double Routine_Calib::Pos(const char* axis) const
    {
        auto a = m_motion.Axis(axis);
        return a ? a->CurrentPos() : 0.0;
    }

    void Routine_Calib::Log(const std::string& msg, bool warn) const
    {
        LogWorker::Instance().RecordLog(msg, warn ? LogLevel::Warning : LogLevel::Info, "Calib");
    }
}
