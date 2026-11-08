#include "Controller/VmCycle.h"

#include <cmath>
#include <format>

#include "Base/Text.h"

namespace scs
{
    std::string VmInspectRead::Describe() const
    {
        if (!CallOk) return CallError.empty() ? std::string("no result") : CallError;
        if (!HasResult) return std::string("output ") + VisionOutputNames::OutResult + " missing or not a number";
        return std::format("{} ({} {})", Pass ? "OK" : "NG", VisionOutputNames::OutResult, VmCycle::Fmt(Result));
    }

    VmCallRequest VmCycle::InspectRequest(const VisionOutputNames& n, int nozzle)
    {
        VmCallRequest req;
        req.Flow = n.InspectFlow();
        req.Module = base::Trim(n.InspectPath);
        req.Camera = EVmCamera::Cam;
        req.Nozzle = nozzle;
        req.Outputs.push_back(VisionOutputNames::OutResult);
        return req;
    }

    VmInspectRead VmCycle::ReadInspect(const VmCallResult& r)
    {
        VmInspectRead c;
        if (!r.Ok) { c.CallError = r.Error.empty() ? "no result" : r.Error; return c; }
        c.CallOk = true;
        c.Result = r.Get(VisionOutputNames::OutResult);
        c.HasResult = VmMath::IsFinite(c.Result);
        c.Pass = c.HasResult && VmMath::SameInt(c.Result, 1);
        return c;
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
