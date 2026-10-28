#include "Controller/VmCycle.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

#include "Infor/AxisId.h"

namespace scs
{
    namespace
    {
        double Pos(const MotionConnect* motion, const char* axis)
        {
            auto a = motion == nullptr ? nullptr : motion->Axis(axis);
            return a == nullptr ? 0 : a->CurrentPos();
        }

        void AddUnique(std::vector<std::string>& names, const std::string& o)
        {
            if (std::find(names.begin(), names.end(), o) == names.end()) names.push_back(o);
        }

        void AddExpect(std::vector<VmFlowExpect>& list, const VmCallRequest& req, const std::string& image, const char* note)
        {
            VmFlowExpect e;
            e.Flow = req.Flow; e.ImageName = image; e.Note = note ? note : "";
            for (const auto& o : req.Outputs) AddUnique(e.Names, o);
            for (const auto& o : req.OptionalOutputs) AddUnique(e.Names, o);
            list.push_back(std::move(e));
        }

        std::string Join(const std::vector<std::string>& parts)
        {
            std::string s;
            for (const auto& p : parts)
            {
                if (!s.empty()) s += "; ";
                s += p;
            }
            return s;
        }

        void CheckEcho(const VmCallResult& r, const std::string& name, double expect, double tol, std::vector<std::string>& issues)
        {
            if (!r.Has(name)) { issues.push_back("missing output " + name); return; }
            double got = r.Get(name);
            if (!VmMath::Near(got, expect, tol)) issues.push_back(name + " = " + VmCycle::Fmt(got) + " ≠ " + VmCycle::Fmt(expect));
        }

        double DaysSince(const std::optional<TimePoint>& at)
        {
            if (!at) return std::numeric_limits<double>::quiet_NaN();
            return std::chrono::duration<double>(std::chrono::system_clock::now() - *at).count() / 86400.0;
        }
    }

    std::string VmCalibGate::Describe() const
    {
        return Block.empty() ? std::string("Calib: ready") : "Calib: NOT ready — " + Block;
    }

    // ───────────────────────── biến App ghi ─────────────────────────

    std::string VmCycle::AxisVarProblem(const VisionOutputNames* n)
    {
        if (n == nullptr) return "vision name config missing";
        if (n->GvAxisX.empty() || n->GvAxisZ.empty())
            return "axis variable names (X, Z) empty on Setting page";
        return {};
    }

    std::map<std::string, double> VmCycle::AxisGlobals(const MotionConnect* motion, const VisionOutputNames& n, int runId, int nozzle)
    {
        std::map<std::string, double> g;
        g[n.GvAxisX] = Pos(motion, AxisId::X);
        g[n.GvAxisZ] = Pos(motion, AxisId::Z);
        if (!n.GvNozzle.empty()) g[n.GvNozzle] = nozzle;
        g[VmVars::RunId] = runId;
        return g;
    }

    // ───────────────────────── yêu cầu gọi Flow ─────────────────────────

    VmCallRequest VmCycle::LocateRequest(const VisionOutputNames& n, const std::map<std::string, double>& globals)
    {
        VmCallRequest req;
        req.Flow = n.FlowLocateCan; req.Camera = EVmCamera::Cam; req.Globals = globals;
        if (!n.GvIsLocate.empty()) req.Globals[n.GvIsLocate] = 1;   // nhánh soi
        req.Outputs.push_back(n.OutFound);
        for (const auto& o : VmOut::LocateEcho()) req.Outputs.push_back(o);
        for (const auto& o : VmOut::LocateScript()) req.OptionalOutputs.push_back(o);
        return req;
    }

    // ───────────────────────── hợp đồng Output Params ─────────────────────────

    std::vector<VmFlowExpect> VmCycle::ExpectedFlowOutputs(const VisionOutputNames& n)
    {
        std::vector<VmFlowExpect> list;
        std::map<std::string, double> empty;
        AddExpect(list, LocateRequest(n, empty), n.OutImage,
                  "ObjFound / ObjX_mm / ObjY_mm / ObjRunId are outputs of Script CanAbs — all missing means the Script is not built or not exposed in the Flow's Output Params");
        return list;
    }

    VmFlowOutputs VmCycle::FakeFlowOutputs(const std::string& flow, const VisionOutputNames& n)
    {
        auto expects = ExpectedFlowOutputs(n);
        auto expect = std::find_if(expects.begin(), expects.end(), [&](const VmFlowExpect& e) { return e.Flow == flow; });
        if (expect == expects.end()) return VmFlowOutputs::Fail(flow, "no Flow with this name in contract");

        VmFlowOutputs r;
        r.Flow = flow; r.InputParamCount = 0;
        for (const auto& name : expect->Names)
        {
            VmDeclaredOut d;
            d.Name = name; d.TypeCode = VmOutCheck::TypeFloat; d.TypeText = VmOutCheck::TypeName(VmOutCheck::TypeFloat); d.Numeric = true;
            r.Declared.push_back(std::move(d));
        }

        // Module Script giả — bind đủ, để bản mô phỏng ra báo cáo sạch.
        auto scripts = VmScriptContract::Build(n);
        auto script = std::find_if(scripts.begin(), scripts.end(), [&](const VmScriptExpect& e) { return e.Flow == flow; });
        if (script != scripts.end())
        {
            VmModuleIo m;
            m.Name = script->ScriptName;
            for (const auto& i : script->Inputs) m.Inputs.push_back(VmIoInfo{ i, "float", "simulated" });
            for (const auto& o : script->Outputs) m.Outputs.push_back(VmIoInfo{ o, "float", "" });
            r.Modules.push_back(std::move(m));
        }
        return r;
    }

    // ───────────────────────── đọc kết quả ─────────────────────────

    VmCameraRead VmCycle::ReadLocate(const VmCallResult& r, const std::map<std::string, double>& sent, const VisionOutputNames& n)
    {
        VmCameraRead c;
        c.AxX = Val(sent, n.GvAxisX);
        c.AxZ = Val(sent, n.GvAxisZ);
        if (!r.Ok) { c.CallError = r.Error.empty() ? "no result" : r.Error; return c; }
        c.CallOk = true;

        double rid = Val(sent, VmVars::RunId);
        std::vector<std::string> echo;
        CheckEcho(r, VmOut::EchoRunId, rid, 0.5, echo);
        CheckEcho(r, VmOut::EchoX, c.AxX, VmMath::EchoTol, echo);
        CheckEcho(r, VmOut::EchoZ, c.AxZ, VmMath::EchoTol, echo);
        c.EchoOk = echo.empty();
        c.EchoIssue = Join(echo);

        c.Found = r.Get(n.OutFound) > 0.5;

        std::vector<std::string> vm = echo;
        if (c.Found)
        {
            if (!VmMath::SameInt(r.Get(VmOut::ObjFound), 1)) vm.push_back("ObjFound ≠ 1");
            if (!VmMath::Near(r.Get(VmOut::ObjRunId), rid, 0.5)) vm.push_back("ObjRunId ≠ RunId — Script did not reach last line");
            c.X = r.Get(VmOut::ObjX);
            c.Y = r.Get(VmOut::ObjY);
            if (!VmMath::IsFinite(c.X) || !VmMath::IsFinite(c.Y))
                vm.push_back(std::string(VmOut::ObjX) + "/" + VmOut::ObjY + " non-finite or missing");
            else
            {
                c.RelX = c.X - c.AxX;
                c.RelZ = c.Y - c.AxZ;
            }
        }
        c.VmValid = vm.empty();
        c.VmIssue = Join(vm);
        return c;
    }

    // ───────────────────────── độ lệch và đích bù ─────────────────────────

    VmDeviation VmCycle::Deviation(const NozzleCalib& nozzle, double relX, double relZ, const LaserCompSettings& comp,
                                   double baseX, double baseZ, bool useVision)
    {
        VmDeviation d;
        d.Dx = useVision ? relX - nozzle.RefX : 0.0;
        d.Dz = useVision ? relZ - nozzle.RefZ : 0.0;
        d.TargetX = baseX + nozzle.TrimX + comp.GainX * d.Dx;
        d.TargetZ = baseZ + nozzle.TrimZ + comp.GainZ * d.Dz;
        if (!VmMath::IsFinite(d.Dx) || !VmMath::IsFinite(d.Dz) || !VmMath::IsFinite(d.TargetX) || !VmMath::IsFinite(d.TargetZ))
        {
            d.Why = "deviation or target non-finite";
            return d;
        }
        if (!VmMath::IsFinite(comp.MaxCompMm) || comp.MaxCompMm <= 0)
        {
            d.Why = "invalid MaxCompMm on Setting page";
            return d;
        }
        if (std::fabs(d.Dx) > comp.MaxCompMm || std::fabs(d.Dz) > comp.MaxCompMm)
        {
            d.Why = std::format("deviation X {:+.3f}, Z {:+.3f} mm exceeds limit {} mm", d.Dx, d.Dz, Fmt(comp.MaxCompMm));
            return d;
        }
        d.Ok = true;
        return d;
    }

    // ───────────────────────── App tự phán ─────────────────────────

    VmGridJudge VmCycle::JudgeGrid(const VmMeanCapture& s0, const VmMeanCapture& s1, const VmMeanCapture& s2, double tolMm)
    {
        VmGridJudge j;
        j.StepX = s1.AxX - s0.AxX;
        j.StepZ = s2.AxZ - s0.AxZ;
        // Rel = Obj − encoder phải đứng yên: can dính trên nozzle nên đi theo trục.
        j.ErrX = s1.RelX - s0.RelX;
        j.CrossX = s1.RelZ - s0.RelZ;
        j.ErrZ = s2.RelZ - s0.RelZ;
        j.CrossZ = s2.RelX - s0.RelX;

        bool finite = VmMath::IsFinite(j.ErrX) && VmMath::IsFinite(j.ErrZ) && VmMath::IsFinite(j.CrossX) &&
                      VmMath::IsFinite(j.CrossZ) && VmMath::IsFinite(j.StepX) && VmMath::IsFinite(j.StepZ) && VmMath::IsFinite(tolMm);
        if (!finite) j.Status = VmStatus::Invalid;
        else if (std::fabs(s1.AxZ - s0.AxZ) > 0.01 || std::fabs(s2.AxX - s0.AxX) > 0.01) j.Status = VmStatus::AxisMoved;
        else if (std::fabs(j.StepX) < 0.5 || std::fabs(j.StepZ) < 0.5) j.Status = VmStatus::NoConclusion;
        else if (std::fabs(j.ErrX) >= std::fabs(j.StepX) || std::fabs(j.ErrZ) >= std::fabs(j.StepZ)) j.Status = VmStatus::Code8;
        else if (std::fabs(j.ErrX) > tolMm || std::fabs(j.ErrZ) > tolMm || std::fabs(j.CrossX) > tolMm || std::fabs(j.CrossZ) > tolMm)
            j.Status = VmStatus::EccOut;
        else j.Status = VmStatus::Ok;
        return j;
    }

    // ───────────────────────── cổng hiệu chuẩn ─────────────────────────

    VmCalibGate VmCycle::EvaluateCalib(const ModelData* model, const SystemData* sys, int nozzle)
    {
        VmCalibGate g;
        if (model == nullptr || sys == nullptr) { g.Block = "model or config not loaded"; return g; }

        const VisionCalib& c = model->Calib;
        std::vector<int> nozzles;
        if (nozzle >= 1 && nozzle <= VisionCalib::NozzleCount) nozzles.push_back(nozzle);
        else for (int k = 1; k <= VisionCalib::NozzleCount; ++k) nozzles.push_back(k);

        std::vector<std::string> why;
        if (!c.GridVerified()) why.push_back("grid check not passed");

        std::vector<int> noRef;
        for (int k : nozzles)
            if (!c.Nozzle(k).RefMeasured()) noRef.push_back(k);
        if (!noRef.empty())
        {
            std::string t;
            for (int k : noRef) t += (t.empty() ? "" : ", ") + std::to_string(k);
            why.push_back("nozzle " + t + " master not set");
        }

        const auto& comp = sys->LaserComp;
        if (!(comp.MaxCompMm > 0 && comp.MaxCompMm <= 10) || !VmMath::IsFinite(comp.GainX) || !VmMath::IsFinite(comp.GainZ))
            why.push_back("invalid compensation limits on Setting page (MaxCompMm, GainX, GainZ)");

        double oldest = DaysSince(c.GridVerifiedAt);
        for (int k : nozzles)
        {
            double d = DaysSince(c.Nozzle(k).RefAt);
            if (VmMath::IsFinite(d)) oldest = VmMath::IsFinite(oldest) ? std::max(oldest, d) : d;
        }
        g.DaysSinceOldest = oldest;
        g.Block = Join(why);
        return g;
    }

    // ───────────────────────── tiện ích ─────────────────────────

    double VmCycle::Val(const std::map<std::string, double>& d, const std::string& key)
    {
        auto it = d.find(key);
        return it != d.end() ? it->second : std::numeric_limits<double>::quiet_NaN();
    }

    std::string VmCycle::Fmt(double v)
    {
        if (!VmMath::IsFinite(v)) return std::isnan(v) ? std::string("NaN (missing)") : (v > 0 ? std::string("∞") : std::string("-∞"));
        std::string s = std::format("{:.3f}", v);
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s == "-0" ? "0" : s;
    }
}
