#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace ecm
{
    /// <summary>
    /// Live state of one module plugged into a modular I/O coupler's rack (e.g. one slot of a
    /// Leadshine R1EC), refreshed each cycle from that slot's own IORegister. Unlike
    /// IoModuleState.Inputs/Outputs (one register spanning every module on the coupler), each
    /// IoSlotState carries only its own module's channels. Các trường cấu hình gán trước khi chia sẻ;
    /// hai thanh ghi sống là std::atomic.
    /// </summary>
    class IoSlotState
    {
    public:
        IoSlotState() = default;
        IoSlotState(const IoSlotState&) = delete;
        IoSlotState& operator=(const IoSlotState&) = delete;

        /// <summary>Position within the coupler's rack, in the order 0xF050 reported it.</summary>
        int SlotIndex = 0;
        /// <summary>ESI ModuleIdent of the module plugged into this slot (e.g. 0x71101045).</summary>
        uint32_t ModuleIdent = 0;
        /// <summary>Display name resolved from the ESI module catalog, or "Unknown module 0x...".</summary>
        std::string ModuleName;
        /// <summary>Number of digital input/output channels this slot's own module maps.</summary>
        int InputCount = 0;
        int OutputCount = 0;

        /// <summary>Live digital input bits for just this module, channel N in bit N.</summary>
        std::atomic<uint32_t> InputRegister{ 0 };
        /// <summary>Last digital output bits written for just this module — tracked so SetSlotOutput
        /// can flip one channel via read-modify-write without a round trip to the coupler.</summary>
        std::atomic<uint32_t> OutputRegister{ 0 };
    };
}
