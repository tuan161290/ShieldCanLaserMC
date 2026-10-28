#include "EtherCat/Constants/CiA402Registers.h"

#include <format>

namespace ecm
{
    std::string CiA402Registers::DecodeState(uint16_t statusword)
    {
        int masked = statusword & 0x006F;
        if ((statusword & Statusword::Fault) != 0 &&
            (statusword & (Statusword::SwitchOnDisabled | Statusword::OperationEnabled)) == 0)
            return "Fault";
        switch (masked)
        {
            case 0x00: return "Not ready to switch on";
            case 0x40: return "Switch on disabled";
            case 0x21: return "Ready to switch on";
            case 0x23: return "Switched on";
            case 0x27: return "Operation enabled";
            case 0x07: return "Quick stop active";
            case 0x0F: return "Fault reaction active";
            case 0x08: return "Fault";
            default: return std::format("Unknown (0x{:04X})", statusword);
        }
    }

    const char* ToString(OperationMode mode)
    {
        switch (mode)
        {
            case OperationMode::None: return "None";
            case OperationMode::ProfilePosition: return "ProfilePosition";
            case OperationMode::ProfileVelocity: return "ProfileVelocity";
            case OperationMode::ProfileTorque: return "ProfileTorque";
            case OperationMode::Homing: return "Homing";
            case OperationMode::InterpolatedPosition: return "InterpolatedPosition";
            case OperationMode::CyclicSyncPosition: return "CyclicSyncPosition";
            case OperationMode::CyclicSyncVelocity: return "CyclicSyncVelocity";
            case OperationMode::CyclicSyncTorque: return "CyclicSyncTorque";
        }
        return "";
    }
}
