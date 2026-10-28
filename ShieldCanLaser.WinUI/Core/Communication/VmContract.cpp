#include "Communication/VmContract.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <random>

namespace scs
{
    double VmCallResult::Get(const std::string& name) const
    {
        auto it = Values.find(name);
        return it != Values.end() ? it->second : std::numeric_limits<double>::quiet_NaN();
    }

    int VmCallResult::GetInt(const std::string& name) const
    {
        double v = Get(name);
        if (!VmMath::IsFinite(v)) return std::numeric_limits<int>::min();
        double r = std::nearbyint(v);   // Math.Round: làm tròn nửa về số chẵn
        if (r < -2147483648.0 || r > 2147483647.0) return std::numeric_limits<int>::min();
        return static_cast<int>(r);
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

    bool VmMath::Near(double a, double b, double tol)
    {
        return IsFinite(a) && IsFinite(b) && std::fabs(a - b) <= tol;
    }

    bool VmMath::SameInt(double a, int b) { return IsFinite(a) && std::fabs(a - b) < 0.5; }

    int VmRunIds::Next()
    {
        static std::atomic<int> last = []
        {
            std::random_device rd;
            std::uniform_int_distribution<int> dist(1, Max - 1);   // Random.Next(1, Max): cận trên không lấy
            return dist(rd);
        }();

        while (true)
        {
            int cur = last.load();
            int next = cur >= Max ? 1 : cur + 1;
            if (last.compare_exchange_weak(cur, next)) return next;
        }
    }

    const std::vector<std::string>& VmOut::LocateEcho()
    {
        static const std::vector<std::string> v{ EchoRunId, EchoX, EchoZ };
        return v;
    }

    const std::vector<std::string>& VmOut::LocateScript()
    {
        static const std::vector<std::string> v{ ObjFound, ObjX, ObjY, ObjRunId };
        return v;
    }

    std::string VmStatus::GridVerify(int st)
    {
        switch (st)
        {
        case Ok: return "OK";
        case NoConclusion: return "travel below 0.5 mm";
        case EccOut: return "deviation exceeds tolerance";
        case Invalid: return "non-finite value";
        case AxisMoved: return "other axis moved during measurement";
        case Code8: return "WRONG SCALE/SIGN — measured position does not follow the axis";
        default: return "Unknown Status (" + std::to_string(st) + ")";
        }
    }
}
