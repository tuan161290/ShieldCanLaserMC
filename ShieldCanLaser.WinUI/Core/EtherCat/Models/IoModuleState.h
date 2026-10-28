#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EtherCat/Models/IoSlotState.h"

namespace ecm
{
    /// <summary>
    /// Per-I/O-module runtime state, refreshed each cycle by EtherCatMasterService from the module's
    /// PDO (plain digital in/out bits). Name/SlavePosition/InputCount/OutputCount/Slots được gán
    /// trước khi chia sẻ và không đổi về sau; các giá trị sống là std::atomic.
    /// </summary>
    class IoModuleState
    {
    public:
        IoModuleState() = default;
        IoModuleState(const IoModuleState&) = delete;
        IoModuleState& operator=(const IoModuleState&) = delete;

        /// <summary>Display name (from IoModuleConfig)</summary>
        std::string Name;
        /// <summary>EtherCAT slave position (from IoModuleConfig)</summary>
        int SlavePosition = 0;
        /// <summary>Number of digital input/output channels this module actually maps.</summary>
        int InputCount = 0;
        int OutputCount = 0;

        /// <summary>Live digital input bits, channel N in bit N. 64-bit — a module can map up to
        /// 64 DI (e.g. a Leadshine R1EC with two SC-3200-1 expansion modules).</summary>
        std::atomic<uint64_t> Inputs{ 0 };
        /// <summary>Last digital output bits written, channel N in bit N — tracked here so
        /// SetDigitalOutput can flip one channel via read-modify-write.</summary>
        std::atomic<uint64_t> Outputs{ 0 };
        /// <summary>Raw 0xF100:01 Device status of the I/O coupler (0 = OK / not mapped).</summary>
        std::atomic<uint32_t> DeviceStatus{ 0 };

        /// <summary>One entry per module physically plugged into this coupler's rack (0xF050 order).
        /// Empty for a non-modular I/O slave (Inputs/Outputs are still valid there).</summary>
        std::vector<std::shared_ptr<IoSlotState>> Slots;
    };
}
