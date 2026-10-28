#pragma once
#include <cstdint>
#include <string>

namespace ecm
{
    /// <summary>
    /// Per-slot configuration for a digital I/O remote module (e.g. Fastech Ezi-IO, Leadshine R1EC)
    /// — same identity fields as AxisConfig's "Expected driver identity" section, no motion config.
    /// </summary>
    struct IoModuleConfig
    {
        /// <summary>Display name for this module (e.g. "IO_1").</summary>
        std::string Name = "IO 1";
        /// <summary>EtherCAT slave position on the bus (0-based, daisy-chain order).</summary>
        int SlavePosition = 0;
        /// <summary>Expected identity (0x1018:1/:2/:3). Revision is not enforced — display only.</summary>
        uint32_t ExpectedVendorId = 0;
        uint32_t ExpectedProductCode = 0;
        uint32_t ExpectedRevision = 0;
        /// <summary>Display name of the assigned driver — not used for identity matching.</summary>
        std::string ExpectedDriverName;

        /// <summary>Whether this slot has a driver assigned. A slot with ExpectedProductCode == 0 is
        /// "Unconfigured" and is skipped by ConnectAllAxis.</summary>
        bool IsConfigured() const { return ExpectedProductCode != 0; }
    };
}
