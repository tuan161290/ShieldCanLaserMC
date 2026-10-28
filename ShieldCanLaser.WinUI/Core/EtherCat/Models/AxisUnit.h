#pragma once

namespace ecm
{
    /// <summary>
    /// Physical engineering unit an axis's CountsPerUnit/TravelPerRotationUnits values are expressed
    /// in. Purely descriptive — the drive has no matching CiA402 object; EtherCatMasterService always
    /// works in raw encoder counts via AxisConfig.CountsPerUnit. Lowercase member names since this
    /// enum's ToString() drives the UI directly.
    /// </summary>
    enum class AxisUnit
    {
        pulse = 0,
        mm = 1,
        inch = 2,
        degree = 3
    };

    inline const char* ToString(AxisUnit u)
    {
        switch (u)
        {
            case AxisUnit::pulse: return "pulse";
            case AxisUnit::mm: return "mm";
            case AxisUnit::inch: return "inch";
            case AxisUnit::degree: return "degree";
        }
        return "";
    }
}
