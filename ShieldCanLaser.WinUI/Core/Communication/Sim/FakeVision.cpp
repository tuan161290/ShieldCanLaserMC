#include "Communication/Sim/FakeVision.h"

#include "Communication/VisionConnect.h"

#include "Base/Async.h"
#include "Base/Text.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace scs
{
    namespace
    {
        const double NaN = std::numeric_limits<double>::quiet_NaN();

        double F32(double v) { return static_cast<float>(v); }
        bool Fin(double v) { return !std::isnan(v) && !std::isinf(v); }

        /// ToString("R", InvariantCulture): chuỗi ngắn nhất đọc lại ra đúng số đó.
        template <class T>
        std::string RoundTrip(T v)
        {
            if (std::isnan(v)) return "NaN";
            if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
            char buf[64];
            auto r = std::to_chars(buf, buf + sizeof(buf), v);
            return std::string(buf, r.ptr);
        }

        /// double.TryParse(NumberStyles.Float, InvariantCulture); không đọc được thì NaN.
        double ParseInvariant(const std::string& text)
        {
            std::string t = base::Trim(text);
            if (t.empty()) return NaN;
            if (t == "NaN") return NaN;
            if (t == "Infinity") return std::numeric_limits<double>::infinity();
            if (t == "-Infinity") return -std::numeric_limits<double>::infinity();
            const char* first = t.data();
            const char* last = t.data() + t.size();
            if (*first == '+') ++first;
            double d = 0;
            auto r = std::from_chars(first, last, d);
            return r.ec == std::errc() && r.ptr == last ? d : NaN;
        }
    }

    // ── System.Random của .NET Framework (bộ sinh trừ của Knuth) — cùng hạt giống thì cùng dãy số với bản C# ──

    FakeVision::DotNetRandom::DotNetRandom(int seed)
    {
        const int MBIG = 2147483647, MSEED = 161803398;
        int subtraction = seed == std::numeric_limits<int>::min() ? MBIG : std::abs(seed);
        int mj = MSEED - subtraction;
        m_seedArray[55] = mj;
        int mk = 1;
        for (int i = 1; i < 55; i++)
        {
            int ii = (21 * i) % 55;
            m_seedArray[ii] = mk;
            mk = mj - mk;
            if (mk < 0) mk += MBIG;
            mj = m_seedArray[ii];
        }
        for (int k = 1; k < 5; k++)
        {
            for (int i = 1; i < 56; i++)
            {
                m_seedArray[i] = static_cast<int>(static_cast<unsigned>(m_seedArray[i]) - static_cast<unsigned>(m_seedArray[1 + (i + 30) % 55]));
                if (m_seedArray[i] < 0) m_seedArray[i] += MBIG;
            }
        }
        m_inext = 0;
        m_inextp = 21;
    }

    double FakeVision::DotNetRandom::NextDouble()
    {
        const int MBIG = 2147483647;
        int locINext = m_inext, locINextp = m_inextp;
        if (++locINext >= 56) locINext = 1;
        if (++locINextp >= 56) locINextp = 1;
        int retVal = m_seedArray[locINext] - m_seedArray[locINextp];
        if (retVal == MBIG) retVal--;
        if (retVal < 0) retVal += MBIG;
        m_seedArray[locINext] = retVal;
        m_inext = locINext;
        m_inextp = locINextp;
        return retVal * (1.0 / MBIG);
    }

    FakeVision::FakeVision() : m_rng(20260731) {}

    std::optional<AxisSnapshot> FakeVision::LastCalibAxes() const { std::lock_guard<std::recursive_mutex> l(m_lock); return m_lastCalibAxes; }
    void FakeVision::SetNames(std::function<VisionOutputNames()> names) { std::lock_guard<std::recursive_mutex> l(m_lock); m_names = std::move(names); }
    void FakeVision::SetFlowOutputSource(std::function<VmFlowOutputs(const std::string&)> source) { std::lock_guard<std::recursive_mutex> l(m_lock); m_flowOutputSource = std::move(source); }
    void FakeVision::SetAfterFlow(std::function<void(const std::string&)> afterFlow) { std::lock_guard<std::recursive_mutex> l(m_lock); m_afterFlow = std::move(afterFlow); }

    void FakeVision::SetNozzleDeviation(int nozzle, double dx, double dz)
    {
        if (nozzle < 1 || nozzle > NozzleCount) return;
        std::lock_guard<std::recursive_mutex> l(m_lock);
        m_deviation[(size_t)nozzle] = { dx, dz };
    }

    std::pair<double, double> FakeVision::NozzleDeviation(int nozzle) const
    {
        if (nozzle < 1 || nozzle > NozzleCount) return { 0, 0 };
        std::lock_guard<std::recursive_mutex> l(m_lock);
        return m_deviation[(size_t)nozzle];
    }

    bool FakeVision::Connect() { m_connected = true; return true; }

    void FakeVision::Disconnect() { m_connected = false; m_loaded = false; }

    bool FakeVision::LoadSolution(const std::string&)
    {
        base::Delay(200);
        m_loaded = m_connected.load();
        return m_loaded.load();
    }

    VisionPose FakeVision::TriggerCalib(EVisionProcedure procedure, const AxisSnapshot& axes, std::stop_token ct)
    {
        if (!IsConnected()) return VisionPose::NotFound("Not connected");
        if (!IsSolutionLoaded()) return VisionPose::NotFound("Solution not loaded");

        std::function<void(const std::string&)> afterFlow;
        {
            std::lock_guard<std::recursive_mutex> l(m_lock);
            m_lastCalibAxes = axes;
            afterFlow = m_afterFlow;
            // Như thiết bị thật: vị trí trục + biến rẽ nhánh = 0 ghi vào kho biến trước khi chạy Flow.
            VisionOutputNames n = m_names ? m_names() : VisionOutputNames();
            for (auto& kv : VisionConnect::CalibVars(n, axes)) SetG(kv.first, kv.second);
        }
        m_lastNozzle = axes.Nozzle;
        ++m_calibPointCount;
        ++m_pointsSinceClear;
        if (afterFlow) afterFlow(ToString(procedure));   // bài tự kiểm soi trạng thái máy NGAY LÚC chụp (đèn CAM)
        base::Delay(ProcessTimeMs(), ct);

        std::lock_guard<std::recursive_mutex> l(m_lock);
        VisionPose found;
        found.Found = true;
        return m_rng.NextDouble() <= FoundRate() ? found : VisionPose::NotFound("Object not found");
    }

    CalibClearResult FakeVision::ClearCalibPoints(EVisionProcedure)
    {
        if (!IsConnected()) return CalibClearResult::Fail("Not connected");
        if (!IsSolutionLoaded()) return CalibClearResult::Fail("Solution not loaded");
        if (FailClearPoint()) return CalibClearResult::Fail("Flow has no N-point / Translation Calibration module (simulated)");
        ++m_clearPointCount;
        m_pointsSinceClear = 0;
        CalibClearResult r;
        r.Modules.push_back("N-point Calibration");
        return r;
    }

    std::map<std::string, FakeGlobalVar> FakeVision::GlobalVarsSnapshot() const { std::lock_guard<std::recursive_mutex> l(m_lock); return m_globalVars; }

    void FakeVision::PutGlobalVar(const std::string& name, const std::string& type, const std::string& value)
    {
        std::lock_guard<std::recursive_mutex> l(m_lock);
        m_globalVars[name] = FakeGlobalVar{ type, value };
    }

    void FakeVision::RemoveGlobalVar(const std::string& name) { std::lock_guard<std::recursive_mutex> l(m_lock); m_globalVars.erase(name); }
    void FakeVision::ClearGlobalVars() { std::lock_guard<std::recursive_mutex> l(m_lock); m_globalVars.clear(); }

    GlobalVarSyncReport FakeVision::SyncGlobalVariables(const std::vector<GlobalVarSpec>& specs, bool apply)
    {
        if (!IsSolutionLoaded()) return GlobalVarSyncReport::Fail("Solution not loaded");

        struct Store : IGlobalVarStore
        {
            FakeVision& v;
            explicit Store(FakeVision& fv) : v(fv) {}

            std::map<std::string, std::string> ReadAll() override
            {
                std::map<std::string, std::string> d;
                for (auto& kv : v.m_globalVars) d[kv.first] = kv.second.Type;
                return d;
            }

            void Add(const std::string& name, const std::string& type, const std::string&) override
            {
                if (v.m_globalVars.count(name)) throw std::runtime_error("name is already exit");
                v.m_globalVars[name] = FakeGlobalVar{ type, type == "string" ? "" : "0" };
            }

            void SetValue(const std::string& name, const std::string&, const std::string& invariantValue) override
            {
                auto it = v.m_globalVars.find(name);
                if (it == v.m_globalVars.end()) throw std::runtime_error("name is not exit");
                it->second.Value = invariantValue;
            }

            std::string Save() override { ++v.m_solutionSaveCount; return ""; }
        };

        std::lock_guard<std::recursive_mutex> l(m_lock);
        Store store(*this);
        return GlobalVarSync::Run(&store, specs, apply);
    }

    VmFlowOutputs FakeVision::ReadFlowOutputs(const std::string& flow)
    {
        if (!IsSolutionLoaded()) return VmFlowOutputs::Fail(flow, "Solution not loaded");
        std::function<VmFlowOutputs(const std::string&)> source;
        { std::lock_guard<std::recursive_mutex> l(m_lock); source = m_flowOutputSource; }
        if (!source) return VmFlowOutputs::Fail(flow, "Simulated: no solution to read Output Params from");
        return source(flow);
    }

    std::string FakeVision::SaveSolution()
    {
        if (!IsSolutionLoaded()) return "Solution not loaded";
        ++m_solutionSaveCount;
        return {};
    }

    double FakeVision::Noise(double amplitude) { return (m_rng.NextDouble() - 0.5) * 2 * amplitude; }

    void FakeVision::SeedVmSolution(const std::vector<GlobalVarSpec>& specs)
    {
        std::lock_guard<std::recursive_mutex> l(m_lock);
        m_globalVars.clear();
        for (auto& s : specs)
            m_globalVars[s.Name] = FakeGlobalVar{ s.Type, !s.InitialValue.empty() ? s.InitialValue : (s.Type == "string" ? "" : "0") };
    }

    double FakeVision::GetGlobal(const std::string& name) const { std::lock_guard<std::recursive_mutex> l(m_lock); return G(name); }
    void FakeVision::SetGlobal(const std::string& name, double value) { std::lock_guard<std::recursive_mutex> l(m_lock); SetG(name, value); }

    std::vector<std::string> FakeVision::FlowHistorySnapshot() const { std::lock_guard<std::recursive_mutex> l(m_lock); return m_flowHistory; }
    void FakeVision::ClearFlowHistory() { std::lock_guard<std::recursive_mutex> l(m_lock); m_flowHistory.clear(); }

    // ═════════════════════════ bản sao Script của solution ═════════════════════════

    VmCallResult FakeVision::CallFlow(const VmCallRequest& req, std::stop_token ct)
    {
        if (req.Flow.empty()) return VmCallResult::Fail("Missing Flow name");
        if (!IsConnected()) return VmCallResult::Fail("Not connected");
        if (!IsSolutionLoaded()) return VmCallResult::Fail("Solution not loaded");

        std::function<VisionOutputNames()> names;
        std::function<void(const std::string&)> afterFlow;
        { std::lock_guard<std::recursive_mutex> l(m_lock); names = m_names; afterFlow = m_afterFlow; }
        VisionOutputNames n = names ? names() : VisionOutputNames();
        if (req.Flow != n.FlowLocateCan) return VmCallResult::Fail("Flow \"" + req.Flow + "\" not found in solution");

        base::Delay(ProcessTimeMs(), ct);   // Flow có camera

        std::map<std::string, double> o;
        {
            std::lock_guard<std::recursive_mutex> l(m_lock);
            for (auto& kv : req.Globals)
            {
                if (kv.first.empty()) return VmCallResult::Fail("Empty global variable name");
                if (!m_globalVars.count(kv.first))
                    return VmCallResult::Fail("Global variable \"" + kv.first + "\" not declared in solution");
                if (!Fin(kv.second)) return VmCallResult::Fail("Value for \"" + kv.first + "\" is not finite");
            }

            for (auto& kv : req.Globals) SetG(kv.first, kv.second);

            ++m_vmCallCount;
            m_flowHistory.push_back(req.Flow);
            if (m_flowHistory.size() > 5000) m_flowHistory.erase(m_flowHistory.begin());
            m_lastNozzle = VisionConnect::NozzleOf(n, req.Globals);

            if (!n.GvIsLocate.empty() && m_globalVars.count(n.GvIsLocate) && Gi(n.GvIsLocate) == 0)
            {
                // Biến rẽ nhánh = 0 thì Flow chạy nhánh hiệu chuẩn: module nạp thêm một điểm, chỉ ra Found — đúng cái
                // giá phải trả nếu App quên ghi 1 trước lần soi.
                ++m_calibPointCount;
                ++m_pointsSinceClear;
                o[n.OutFound] = 1;
            }
            else RunLocateCan(n, o);
        }

        if (afterFlow) afterFlow(req.Flow);

        VmCallResult res;
        res.Ok = true;
        for (auto& name : req.Outputs)
        {
            auto it = o.find(name);
            if (it == o.end()) return VmCallResult::Fail("Flow \"" + req.Flow + "\" missing output \"" + name + "\"");
            res.Values[name] = it->second;
        }
        for (auto& name : req.OptionalOutputs)
        {
            auto it = o.find(name);
            if (it != o.end()) res.Values[name] = it->second;
            else res.MissingOptional.push_back(name);
        }
        return res;
    }

    /// LocateCan: echo biến vừa ghi; vị trí can = vị trí trục + độ lệch giả của nozzle + nhiễu (hệ vật lý của file hiệu
    /// chuẩn By Coordinate trùng hệ máy).
    void FakeVision::RunLocateCan(const VisionOutputNames& n, std::map<std::string, double>& o)
    {
        ++m_locateCount;

        double x = G(n.GvAxisX), z = G(n.GvAxisZ), rid = G(VmVars::RunId);
        int nozzle = n.GvNozzle.empty() ? 0 : Gi(n.GvNozzle);
        o[VmOut::EchoRunId] = F32(rid);
        o[VmOut::EchoX] = F32(x);
        o[VmOut::EchoZ] = F32(z);

        bool found = m_rng.NextDouble() <= FoundRate();
        auto dev = nozzle >= 1 && nozzle <= NozzleCount ? m_deviation[(size_t)nozzle] : std::pair<double, double>{ 0, 0 };
        double ox = x + dev.first + Noise(NoiseMm());
        double oy = z + dev.second + Noise(NoiseMm());

        o[n.OutFound] = found ? 1 : 0;
        o[VmOut::ObjFound] = found ? 1 : 0;
        o[VmOut::ObjX] = F32(found ? ox : 0);
        o[VmOut::ObjY] = F32(found ? oy : 0);
        o[VmOut::ObjRunId] = F32(rid);

        if (CorruptEchoOnce())
        {
            SetCorruptEchoOnce(false);
            o[VmOut::EchoRunId] = o[VmOut::EchoRunId] + 1;
        }
    }

    // ── kho biến (gọi trong m_lock) ──

    double FakeVision::G(const std::string& name) const
    {
        auto it = m_globalVars.find(name);
        return it == m_globalVars.end() ? NaN : ParseInvariant(it->second.Value);
    }

    int FakeVision::Gi(const std::string& name) const
    {
        double d = G(name);
        return Fin(d) ? static_cast<int>(std::nearbyint(d)) : 0;
    }

    void FakeVision::SetG(const std::string& name, double value)
    {
        auto it = m_globalVars.find(name);
        if (it == m_globalVars.end()) return;
        const std::string& type = it->second.Type;
        it->second.Value = type == "int" ? std::to_string(static_cast<int>(std::nearbyint(value)))
                         : type == "string" ? RoundTrip(value) : RoundTrip(static_cast<float>(value));
    }
}
