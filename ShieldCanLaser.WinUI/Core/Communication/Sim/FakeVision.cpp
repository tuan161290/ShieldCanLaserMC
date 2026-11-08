#include "Communication/Sim/FakeVision.h"

#include "Base/Async.h"
#include "Base/Text.h"

#include <cstdlib>
#include <limits>

namespace scs
{
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

    void FakeVision::SetNames(std::function<VisionOutputNames()> names) { std::lock_guard<std::recursive_mutex> l(m_lock); m_names = std::move(names); }
    void FakeVision::SetAfterFlow(std::function<void(int)> afterFlow) { std::lock_guard<std::recursive_mutex> l(m_lock); m_afterFlow = std::move(afterFlow); }

    VisionOutputNames FakeVision::Names() const
    {
        std::function<VisionOutputNames()> names;
        { std::lock_guard<std::recursive_mutex> l(m_lock); names = m_names; }
        return names ? names() : VisionOutputNames();
    }

    void FakeVision::SetNozzleResult(int nozzle, std::optional<double> result)
    {
        if (nozzle < 1 || nozzle > NozzleCount) return;
        std::lock_guard<std::recursive_mutex> l(m_lock);
        m_nozzleResult[(size_t)nozzle] = result;
    }

    void FakeVision::ClearNozzleResults()
    {
        std::lock_guard<std::recursive_mutex> l(m_lock);
        m_nozzleResult.fill(std::nullopt);
    }

    bool FakeVision::Connect() { m_connected = true; return true; }

    void FakeVision::Disconnect() { m_connected = false; m_loaded = false; }

    bool FakeVision::LoadSolution(const std::string&)
    {
        base::Delay(200);
        m_loaded = m_connected.load();
        return m_loaded.load();
    }

    std::string FakeVision::CheckModulePath(const std::string& path)
    {
        if (!IsSolutionLoaded()) return "Solution not loaded";
        VisionOutputNames n = Names();
        std::string p = base::Trim(path);
        std::string flow = base::Trim(p.substr(0, p.find('.')));
        if (flow != n.InspectFlow()) return "Flow \"" + flow + "\" not found in solution (simulated)";
        if (p != base::Trim(n.InspectPath)) return "Module \"" + p + "\" not found in Flow \"" + flow + "\" (simulated)";
        return {};
    }

    std::string FakeVision::SaveSolution()
    {
        if (!IsSolutionLoaded()) return "Solution not loaded";
        ++m_solutionSaveCount;
        return {};
    }

    std::vector<int> FakeVision::CallHistorySnapshot() const { std::lock_guard<std::recursive_mutex> l(m_lock); return m_callHistory; }
    void FakeVision::ClearCallHistory() { std::lock_guard<std::recursive_mutex> l(m_lock); m_callHistory.clear(); }

    VmCallResult FakeVision::CallFlow(const VmCallRequest& req, std::stop_token ct)
    {
        if (req.Flow.empty()) return VmCallResult::Fail("Missing Flow name");
        if (!IsConnected()) return VmCallResult::Fail("Not connected");
        if (!IsSolutionLoaded()) return VmCallResult::Fail("Solution not loaded");

        VisionOutputNames n = Names();
        if (req.Flow != n.InspectFlow()) return VmCallResult::Fail("Flow \"" + req.Flow + "\" not found in solution");
        if (!req.Module.empty() && req.Module != base::Trim(n.InspectPath))
            return VmCallResult::Fail("Module \"" + req.Module + "\" not found in Flow \"" + req.Flow + "\"");

        base::Delay(ProcessTimeMs(), ct);   // Flow có camera

        double result = 0;
        std::function<void(int)> afterFlow;
        {
            std::lock_guard<std::recursive_mutex> l(m_lock);
            afterFlow = m_afterFlow;
            ++m_inspectCount;
            m_callHistory.push_back(req.Nozzle);
            if (m_callHistory.size() > 5000) m_callHistory.erase(m_callHistory.begin());
            m_lastNozzle = req.Nozzle;
            if (req.Nozzle >= 1 && req.Nozzle <= NozzleCount && m_nozzleResult[(size_t)req.Nozzle]) result = *m_nozzleResult[(size_t)req.Nozzle];
            else result = m_rng.NextDouble() < PassRate() ? 1 : 0;
        }

        if (afterFlow) afterFlow(req.Nozzle);
        if (m_FailNextCall.exchange(false)) return VmCallResult::Fail("Flow \"" + req.Flow + "\" failed (simulated)");

        const bool drop = m_DropResultOnce.exchange(false);
        VmCallResult res;
        res.Ok = true;
        for (auto& name : req.Outputs)
        {
            if (name != VisionOutputNames::OutResult || drop)
                return VmCallResult::Fail("\"" + (req.Module.empty() ? req.Flow : req.Module) + "\" missing output \"" + name + "\"");
            res.Values[name] = result;
        }
        return res;
    }
}
