#include "Communication/VmContract.h"

#include <cmath>
#include <limits>

namespace scs
{
    double VmCallResult::Get(const std::string& name) const
    {
        auto it = Values.find(name);
        return it != Values.end() ? it->second : std::numeric_limits<double>::quiet_NaN();
    }

    VmCallResult VmCallResult::Fail(const std::string& why, bool timedOut)
    {
        VmCallResult r;
        r.Ok = false;
        r.Error = why;
        r.TimedOut = timedOut;
        return r;
    }

    bool VmMath::IsFinite(double v) { return !std::isnan(v) && !std::isinf(v); }

    bool VmMath::SameInt(double a, int b) { return IsFinite(a) && std::fabs(a - b) < 0.5; }
}
