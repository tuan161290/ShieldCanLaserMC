#include "Controller/McStatus.h"

namespace scs
{
    std::string McStatus::AutoStepText() const
    {
        EAutoState s = AutoStep();
        switch (s)
        {
            case EAutoState::S00_Ready:     return "S00 · Ready";
            case EAutoState::S10_WaitStart: return "S10 · Wait START";
            case EAutoState::S20_PickRow:   return "S20 · Pick row of 7 Shield CAN";
            case EAutoState::S30_Vision:    return "S30 · CAM inspect each nozzle";
            case EAutoState::S40_Laser:     return "S40 · Laser marking each nozzle";
            case EAutoState::S50_PlaceRow:  return "S50 · Return row to tray";
            case EAutoState::S60_WaitTray:  return "S60 · Trays done — replace trays, press START";
            default:                        return ToString(s);
        }
    }

    std::string McStatus::StateText() const
    {
        EMcState s = State();
        switch (s)
        {
            case EMcState::Init:         return "INITIALIZING";
            case EMcState::NotHomed:     return "NOT HOMED";
            case EMcState::Homing:       return "HOMING";
            case EMcState::Idle:         return "READY";
            case EMcState::Armed:        return "AUTO READY — WAIT START";
            case EMcState::Running:      return "RUNNING";
            case EMcState::Pausing:      return "STOPPING…";
            case EMcState::Paused:       return "STOPPED";
            case EMcState::Held:         return "PAUSED";
            case EMcState::WaitOperator: return "WAIT OPERATOR";
            case EMcState::Alarm:        return "ALARM";
            case EMcState::Emg:          return "EMERGENCY STOP";
            default:                     return ToString(s);
        }
    }
}
