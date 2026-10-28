#pragma once
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace ecm
{
    /// <summary>
    /// One slave found live on the bus by IEtherCatBus::ScanSlaves — its position and identity
    /// (read from EEPROM/SII without needing PRE-OP or PDO mapping), plus a best-effort SDO read of
    /// encoder resolution (needs the mailbox, so it can be unavailable).
    /// </summary>
    struct DiscoveredSlaveInfo
    {
        int SlavePosition = 0;
        uint32_t VendorId = 0;
        uint32_t ProductCode = 0;
        uint32_t RevisionNo = 0;

        /// <summary>CiA402 0x608F:1 "Encoder increments". 0 if Scan couldn't reach the mailbox or
        /// the drive doesn't declare this object.</summary>
        uint32_t EncoderIncrements = 0;
        /// <summary>CiA402 0x608F:2 "Motor revolutions". 0 if unavailable.</summary>
        uint32_t EncoderMotorRevolutions = 0;

        /// <summary>Encoder increments per single motor revolution ("Pulses per rotation"), or
        /// nullopt if 0x608F wasn't available during Scan.</summary>
        std::optional<double> PulsesPerRotation() const
        {
            if (EncoderMotorRevolutions > 0)
                return static_cast<double>(EncoderIncrements) / EncoderMotorRevolutions;
            return std::nullopt;
        }

        std::string ToString() const
        {
            return std::format("[{}] Vendor 0x{:X} Product 0x{:X} Rev 0x{:X}", SlavePosition, VendorId, ProductCode, RevisionNo);
        }
    };
}
