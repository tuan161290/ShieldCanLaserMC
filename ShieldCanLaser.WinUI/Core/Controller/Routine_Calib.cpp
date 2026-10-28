#include "Controller/Routine_Calib.h"

#include <cmath>
#include <format>

#include "Base/Async.h"
#include "Base/Text.h"
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

    std::optional<Routine_Calib::LastRead> Routine_Calib::Last(int nozzle) const
    {
        if (!ValidNozzle(nozzle)) return std::nullopt;
        std::lock_guard<std::mutex> l(m_mutex);
        return m_last[(size_t)nozzle];
    }

    VmCalibGate Routine_Calib::CheckCalib(int nozzle) const
    {
        auto model = McDataManager::Model();
        auto sys = m_sys();
        return VmCycle::EvaluateCalib(model.get(), sys.get(), nozzle);
    }

    // ═════════════════════════ Lưới ═════════════════════════

    CalibResult Routine_Calib::SaveGridCenter()
    {
        auto sys = m_sys();
        auto& c = sys->VisionCalib;
        c.CenterX = Pos(AxisId::X);
        c.CenterZ = Pos(AxisId::Z);
        c.CenterSet = true;
        bool saved = McDataManager::SaveSystemData();
        std::string text = std::format("Grid center: X {:.3f}, Z {:.3f}", c.CenterX, c.CenterZ);
        Log("Grid calibration — " + text);
        return saved ? CalibResult::Done(text + " — saved") : CalibResult::Fail(text + " — system.json WRITE FAILED, see alarm");
    }

    CalibResult Routine_Calib::RunGridPoint(int number, std::stop_token ct)
    {
        return Guard("Grid point", [&] {
            auto sys = m_sys();
            const auto& c = sys->VisionCalib;
            if (number < 1 || number > 9) return CalibResult::Fail("point number must be 1–9");
            if (!c.CenterSet) return CalibResult::Fail("grid center not set — jog the can to image center, then press \"SET CENTER\"");
            int nozzle = ValidNozzle(c.CalibNozzle) ? c.CalibNozzle : 1;
            if (!Holding(nozzle)) return CalibResult::Fail(std::format("nozzle {} not holding a can — vacuum a master can onto it (Manual page), then retry", nozzle));

            int idx = number - 1;
            double x = c.CenterX + (idx % 3 - 1) * c.GridStepX;
            double z = c.CenterZ + (idx / 3 - 1) * c.GridStepZ;
            if (!MoveTo({ { AxisId::X, x } }, z, ct))
                return CalibResult::Fail(std::format("point {}: X/Z move to ({:.3f}, {:.3f}) failed", number, x, z));
            auto pose = Trigger(nozzle, ct);
            if (!pose.Found)
                return CalibResult::Fail(std::format("point {}: LocateCan (calib branch) can not found{}", number,
                                                     pose.Error.empty() ? "" : " (" + pose.Error + ")"));
            return CalibResult::Done(std::format("Point {} loaded at X {:.3f}, Z {:.3f}", number, x, z));
        });
    }

    CalibResult Routine_Calib::RunGrid(const std::function<void(const std::string&)>& progress, std::stop_token ct)
    {
        if (!m_sys()->VisionCalib.CenterSet) return CalibResult::Fail("grid center not set — jog the can to image center, then press \"SET CENTER\"");
        // Chuỗi mới: xoá điểm cũ của module N-point trước. Không xoá được thì chưa chạy trục nào.
        if (progress) progress("Clear Point on N-Point module…");
        std::string why = ClearPoints();
        if (!why.empty()) return CalibResult::Fail("Clear Point failed — no axis moved: " + why);
        for (size_t i = 0; i < GridSOrder.size(); ++i)
        {
            int n = GridSOrder[i];
            if (progress) progress(std::format("Point {}/9 (no. {})…", i + 1, n));
            auto r = RunGridPoint(n, ct);
            if (!r.Ok) return CalibResult::Fail(std::format("Stopped at point {} (no. {}): {}", i + 1, n, r.Text));
        }
        return CalibResult::Done("Old points cleared, 9 points loaded. In LocateCan N-Point module: Calibrate → Create Calibration File, "
                                 "disable module, then run VERIFY GRID.");
    }

    CalibResult Routine_Calib::TriggerHere(std::stop_token ct)
    {
        return Guard("Capture here", [&] {
            int nozzle = ValidNozzle(m_sys()->VisionCalib.CalibNozzle) ? m_sys()->VisionCalib.CalibNozzle : 1;
            m_func.WaitSettle(ct);
            auto pose = Trigger(nozzle, ct);
            return pose.Found ? CalibResult::Done("Point captured here — added to N-Point list")
                              : CalibResult::Fail("LocateCan (calib branch): can not found" + (pose.Error.empty() ? "" : " (" + pose.Error + ")"));
        });
    }

    CalibResult Routine_Calib::VerifyGrid(std::stop_token ct)
    {
        return Guard("Verify grid", [&]() -> CalibResult {
            auto sys = m_sys();
            const auto& c = sys->VisionCalib;
            double step = c.VerifyStepMm;
            if (std::fabs(step) < 0.5) return CalibResult::Fail("test travel below 0.5 mm — measurement noise would exceed the signal");
            int nozzle = ValidNozzle(c.CalibNozzle) ? c.CalibNozzle : 1;
            if (!Holding(nozzle)) return CalibResult::Fail(std::format("nozzle {} not holding a can", nozzle));
            int shots = std::max(1, c.ShotsPerPoint);
            double x0 = Pos(AxisId::X), z0 = Pos(AxisId::Z);

            auto s0 = CaptureMean(nozzle, shots, ct);
            if (!s0.Ok) return CalibResult::Fail(s0.Reason);

            if (!MoveTo({ { AxisId::X, x0 + step } }, z0, ct)) return CalibResult::Fail("X axis move failed");
            auto s1 = CaptureMean(nozzle, shots, ct);
            bool backX = MoveTo({ { AxisId::X, x0 } }, z0, ct);
            if (!s1.Ok) return CalibResult::Fail(s1.Reason);
            if (!backX) return CalibResult::Fail("X return to start failed");

            if (!MoveTo({}, z0 + step, ct)) return CalibResult::Fail("Z axis move failed");
            auto s2 = CaptureMean(nozzle, shots, ct);
            bool backZ = MoveTo({}, z0, ct);
            if (!s2.Ok) return CalibResult::Fail(s2.Reason);
            if (!backZ) return CalibResult::Fail("Z return to start failed");

            auto j = VmCycle::JudgeGrid(s0, s1, s2, c.ResidualTolMm);
            std::string numbers = std::format("drift X {:.3f}, Z {:.3f} mm · cross-axis {:.3f}/{:.3f} mm", j.ErrX, j.ErrZ, j.CrossX, j.CrossZ);
            Log(std::format("Verify grid (travel {:.1f} mm): Status {}, {} — {}", step, j.Status, numbers,
                            j.Status == VmStatus::Ok ? "PASS" : "FAIL"), j.Status != VmStatus::Ok);
            if (j.Status != VmStatus::Ok)
                return CalibResult::Fail("FAIL — " + VmStatus::GridVerify(j.Status) + " · " + numbers);

            auto model = McDataManager::Model();
            model->Calib.GridVerifiedAt = std::chrono::system_clock::now();
            return CalibResult::Done("PASS — " + numbers + " · " + SaveModelNote());
        });
    }

    // ═════════════════════════ Master từng nozzle ═════════════════════════

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

    CalibResult Routine_Calib::Capture(int nozzle, std::stop_token ct)
    {
        return Guard("Capture", [&]() -> CalibResult {
            if (!ValidNozzle(nozzle)) return CalibResult::Fail("nozzle must be 1–7");
            if (!Holding(nozzle)) return CalibResult::Fail(std::format("nozzle {} not holding a can", nozzle));
            auto m = CaptureMean(nozzle, 1, ct);
            if (!m.Ok) return CalibResult::Fail(m.Reason);
            auto model = McDataManager::Model();
            const auto& nc = model->Calib.Nozzle(nozzle);
            LastRead r{ m.RelX, m.RelZ, m.RelX - nc.RefX, m.RelZ - nc.RefZ };
            {
                std::lock_guard<std::mutex> l(m_mutex);
                m_last[(size_t)nozzle] = r;
            }
            std::string dev = nc.RefMeasured() ? std::format(" · vs. master dX {:+.3f}, dZ {:+.3f} mm", r.Dx, r.Dz) : std::string(" · master not set");
            return CalibResult::Done(std::format("Nozzle {}: Rel X {:+.3f}, Z {:+.3f} mm{}", nozzle, m.RelX, m.RelZ, dev));
        });
    }

    CalibResult Routine_Calib::SetMaster(int nozzle, std::stop_token ct)
    {
        return Guard("Set master", [&]() -> CalibResult {
            if (!ValidNozzle(nozzle)) return CalibResult::Fail("nozzle must be 1–7");
            if (!Holding(nozzle)) return CalibResult::Fail(std::format("nozzle {} not holding a can — vacuum a master can seated correctly, then retry", nozzle));
            auto go = GoToCam(nozzle, ct);
            if (!go.Ok) return go;

            auto sys = m_sys();
            auto m = CaptureMean(nozzle, std::max(1, sys->VisionCalib.ShotsPerPoint), ct);
            if (!m.Ok) return CalibResult::Fail(m.Reason);
            if (!VmMath::IsFinite(m.RelX) || !VmMath::IsFinite(m.RelZ)) return CalibResult::Fail("measurement not finite");

            auto model = McDataManager::Model();
            auto& nc = model->Calib.Nozzle(nozzle);
            bool hadTrim = nc.TrimX != 0 || nc.TrimZ != 0;
            nc.RefX = m.RelX;
            nc.RefZ = m.RelZ;
            nc.RefAt = std::chrono::system_clock::now();
            nc.TrimX = nc.TrimZ = 0;   // trim cũ bù cho master cũ — giữ lại là bù hai lần
            {
                std::lock_guard<std::mutex> l(m_mutex);
                m_last[(size_t)nozzle] = LastRead{ m.RelX, m.RelZ, 0, 0 };
            }
            Log(std::format("Master nozzle {}: Ref X {:+.3f}, Z {:+.3f} mm = Obj ({:.3f}, {:.3f}) − axis ({:.3f}, {:.3f}), mean of {} images",
                            nozzle, m.RelX, m.RelZ, m.X, m.Y, m.AxX, m.AxZ, m.Shots));
            return CalibResult::Done(std::format("Master nozzle {}: Ref X {:+.3f}, Z {:+.3f} mm{} · {}", nozzle, m.RelX, m.RelZ,
                                                 hadTrim ? " · old trim cleared" : "", SaveModelNote()));
        });
    }

    // ═════════════════════════ Thử laser ═════════════════════════

    CalibResult Routine_Calib::GoToLaser(int nozzle, std::stop_token ct)
    {
        return Guard("Go to laser", [&]() -> CalibResult {
            if (!ValidNozzle(nozzle)) return CalibResult::Fail("nozzle must be 1–7");
            std::string why;
            auto p = LaserPoint(nozzle, why);
            if (!p) return CalibResult::Fail(why);
            auto sys = m_sys();
            auto model = McDataManager::Model();
            const auto& nc = model->Calib.Nozzle(nozzle);
            std::optional<LastRead> last;
            {
                std::lock_guard<std::mutex> l(m_mutex);
                last = m_last[(size_t)nozzle];
            }
            bool useVision = last.has_value() && nc.RefMeasured();
            auto dev = VmCycle::Deviation(nc, last ? last->RelX : 0.0, last ? last->RelZ : 0.0, sys->LaserComp,
                                          p->Get(AxisId::X), p->Get(AxisId::Z), useVision);
            if (!dev.Ok) return CalibResult::Fail(dev.Why);
            if (!MoveTo({ { AxisId::X, dev.TargetX } }, dev.TargetZ, ct)) return CalibResult::Fail("X/Z move to laser point failed");
            return CalibResult::Done(std::format("Nozzle {} at laser: X {:.3f}, Z {:.3f} ({}dX {:+.3f}, dZ {:+.3f}, trim {:+.3f}/{:+.3f})",
                                                 nozzle, dev.TargetX, dev.TargetZ, useVision ? "" : "no capture — ", dev.Dx, dev.Dz, nc.TrimX, nc.TrimZ));
        });
    }

    CalibResult Routine_Calib::TestMark(std::stop_token ct)
    {
        return Guard("Test mark", [&]() -> CalibResult {
            auto io = m_func.Io();
            if (!io) return CalibResult::Fail("Remote IO not available");
            if (m_motion.AnyBusy()) return CalibResult::Fail("axis moving — wait until stopped");
            auto model = McDataManager::Model();
            int hold = std::max(model->Auto.MarkTimeMs, m_sys()->LaserComp.LaserPulseMinMs);
            io->SetDo(IoMap::DoLaserTrigger, true);
            struct Off { std::shared_ptr<IIoDevice> io; ~Off() { io->SetDo(IoMap::DoLaserTrigger, false); } } off{ io };
            base::Delay(hold, ct);
            return CalibResult::Done(std::format("Laser pulse {} ms at X {:.3f}, Z {:.3f}", hold, Pos(AxisId::X), Pos(AxisId::Z)));
        });
    }

    std::string Routine_Calib::SetTrim(int nozzle, double x, double z)
    {
        if (!ValidNozzle(nozzle)) return "nozzle must be 1–7";
        if (!VmMath::IsFinite(x) || !VmMath::IsFinite(z)) return "invalid number";
        if (std::fabs(x) > VisionCalib::MaxTrimMm || std::fabs(z) > VisionCalib::MaxTrimMm)
            return std::format("max trim ±{} mm — larger offset is a teach/calibration error, fix at source", VisionCalib::MaxTrimMm);
        auto model = McDataManager::Model();
        auto& nc = model->Calib.Nozzle(nozzle);
        nc.TrimX = x; nc.TrimZ = z;
        Log(std::format("Laser trim nozzle {}: X {:+.3f}, Z {:+.3f} mm", nozzle, x, z));
        if (!McDataManager::ModelLoaded()) return {};
        return McDataManager::SaveModel() ? std::string() : "MODEL SAVE FAILED — see alarm";
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

    VmMeanCapture Routine_Calib::CaptureMean(int nozzle, int shots, std::stop_token ct)
    {
        VmMeanCapture res;
        res.Shots = shots;
        auto sys = m_sys();
        const auto& names = sys->VisionOutputs;
        res.Reason = VmCycle::AxisVarProblem(&names);
        if (!res.Reason.empty()) return res;

        double sx = 0, sy = 0, ax = 0, az = 0;
        for (int i = 1; i <= shots; ++i)
        {
            if (ct.stop_requested()) { res.Reason = "canceled"; return res; }
            m_func.WaitSettle(ct);
            auto g = VmCycle::AxisGlobals(&m_motion, names, VmRunIds::Next(), nozzle);
            auto req = VmCycle::LocateRequest(names, g);
            auto r = Call(req, ct);
            auto read = VmCycle::ReadLocate(r, g, names);

            std::string what = std::format("LocateCan N{}, image {}/{}: ", nozzle, i, shots);
            if (!read.CallOk) { res.Reason = what + read.CallError; return res; }
            if (!read.EchoOk) { res.Reason = what + read.EchoIssue; return res; }
            if (!read.Found) { res.Reason = what + "can not found"; return res; }
            if (!read.VmValid) { res.Reason = what + read.VmIssue; return res; }

            sx += read.X;
            sy += read.Y;
            ax += read.AxX;
            az += read.AxZ;
        }
        res.X = sx / shots;
        res.Y = sy / shots;
        res.AxX = ax / shots;
        res.AxZ = az / shots;
        res.RelX = res.X - res.AxX;
        res.RelZ = res.Y - res.AxZ;
        res.Ok = true;
        Log(std::format("LocateCan N{}, mean of {} images: Obj ({:.3f}, {:.3f}) · axis X {:.3f}, Z {:.3f} · Rel ({:+.3f}, {:+.3f})",
                        nozzle, shots, res.X, res.Y, res.AxX, res.AxZ, res.RelX, res.RelZ));
        return res;
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
        if (io) io->SetDo(IoMap::DoCamLight, on);
    }

    VmCallResult Routine_Calib::Call(const VmCallRequest& req, std::stop_token ct)
    {
        auto vision = m_vision ? m_vision() : nullptr;
        if (!vision) return VmCallResult::Fail("vision device not available");
        Lamp(true);
        struct Off { Routine_Calib* r; ~Off() { r->Lamp(false); } } off{ this };
        base::Delay(m_sys()->LampSettleMs, ct);
        return vision->CallFlow(req, ct);
    }

    VisionPose Routine_Calib::Trigger(int nozzle, std::stop_token ct)
    {
        auto vision = m_vision ? m_vision() : nullptr;
        if (!vision) return VisionPose::NotFound("vision device not available");
        m_func.WaitSettle(ct);
        AxisSnapshot snap;
        snap.X = Pos(AxisId::X);
        snap.Z = Pos(AxisId::Z);
        snap.Nozzle = nozzle;
        Lamp(true);
        struct Off { Routine_Calib* r; ~Off() { r->Lamp(false); } } off{ this };
        base::Delay(m_sys()->LampSettleMs, ct);
        auto pose = vision->TriggerCalib(EVisionProcedure::GridCalib, snap, ct);
        Log(std::format("Grid calib N{}: X {:.3f}, Z {:.3f} — {}", nozzle, snap.X, snap.Z, pose.Found ? "found" : "NOT found"), !pose.Found);
        return pose;
    }

    std::string Routine_Calib::ClearPoints()
    {
        auto vision = m_vision ? m_vision() : nullptr;
        if (!vision) return "vision device not available";
        auto r = vision->ClearCalibPoints(EVisionProcedure::GridCalib);
        if (r.Ok()) Log("Grid calib: Clear Point — " + (r.Modules.empty() ? std::string("(unnamed)") : ::base::Join(r.Modules, ", ")));
        else Log("Grid calib: Clear Point failed — " + r.Error, true);
        return r.Error;
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

    std::string Routine_Calib::SaveModelNote() const
    {
        if (!McDataManager::ModelLoaded()) return "model not loaded from disk — record kept in memory only";
        auto model = McDataManager::Model();
        return McDataManager::SaveModel() ? "saved to model " + (model ? model->Name : std::string()) : "MODEL SAVE FAILED — see alarm";
    }

    void Routine_Calib::Log(const std::string& msg, bool warn) const
    {
        LogWorker::Instance().RecordLog(msg, warn ? LogLevel::Warning : LogLevel::Info, "Calib");
    }
}
