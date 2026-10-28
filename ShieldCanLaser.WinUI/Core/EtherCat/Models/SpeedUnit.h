#pragma once

namespace ecm
{
    /// <summary>
    /// Engineering unit DefaultVelocity is expressed/displayed in for an axis. EtherCatMasterService
    /// converts a typed velocity to units/s with it (see ToUnitsPerSecond) and always sends Profile
    /// Velocity (0x6081) as raw counts/sec via CountsPerUnit.
    /// </summary>
    enum class SpeedUnit
    {
        UnitPerTime = 0,
        Rpm = 1,
        UnitPerMin = 2,
        UnitPerSec = 3
    };

    inline const char* ToString(SpeedUnit u)
    {
        switch (u)
        {
            case SpeedUnit::UnitPerTime: return "UnitPerTime";
            case SpeedUnit::Rpm: return "Rpm";
            case SpeedUnit::UnitPerMin: return "UnitPerMin";
            case SpeedUnit::UnitPerSec: return "UnitPerSec";
        }
        return "";
    }
}
