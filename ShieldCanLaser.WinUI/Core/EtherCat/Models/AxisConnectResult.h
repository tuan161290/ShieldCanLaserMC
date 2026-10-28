#pragma once
#include <cstdint>
#include <format>
#include <string>

#include "EtherCat/Models/AxisAddress.h"

namespace ecm
{
    /// <summary>Per-slot outcome of IEtherCatBus::ConnectAllAxis.</summary>
    enum class AxisConnectStatus
    {
        /// <summary>Slave found at the expected position, VendorId/ProductCode match, and its
        /// default PDO maps everything EtherCatMasterService needs.</summary>
        Ok = 0,
        /// <summary>A slave was found, but its VendorId/ProductCode does not match the driver type
        /// selected for this slot (wrong model, or slaves plugged in a different order).</summary>
        WrongDriver = 1,
        /// <summary>No slave answered at the expected position (unplugged, no power, bad cable, or
        /// the bus itself never reached PRE-OP).</summary>
        NoResponse = 2,
        /// <summary>Identity matched, but the slave's default PDO doesn't map one of the standard
        /// objects EtherCatMasterService needs.</summary>
        PdoUnsupported = 3,
    };

    inline const char* ToString(AxisConnectStatus s)
    {
        switch (s)
        {
            case AxisConnectStatus::Ok: return "Ok";
            case AxisConnectStatus::WrongDriver: return "WrongDriver";
            case AxisConnectStatus::NoResponse: return "NoResponse";
            case AxisConnectStatus::PdoUnsupported: return "PdoUnsupported";
        }
        return "";
    }

    /// <summary>
    /// Validation result for one configured slot, returned by IEtherCatBus::ConnectAllAxis — one
    /// entry per AxisConfig/IoModuleConfig with IsConfigured() true. Lets the config UI show a
    /// ✅/⚠ icon per slot instead of stopping at the first bad slave.
    /// </summary>
    struct AxisConnectResult
    {
        /// <summary>EtherCAT slave position this result is for (matches AxisConfig.SlavePosition).</summary>
        int SlavePosition = 0;
        /// <summary>Kênh trục trong slave (AxisConfig.AxisChannel) — hai trục của một driver WE2x cùng SlavePosition.</summary>
        int AxisChannel = 0;
        /// <summary>Axis display name (from AxisConfig.Name), for UI convenience.</summary>
        std::string AxisName;
        AxisConnectStatus Status = AxisConnectStatus::Ok;
        /// <summary>Human-readable detail — the specific mismatch/timeout, shown in the UI tooltip.</summary>
        std::string Message;
        /// <summary>Identity actually read from the slave (0 if none found). Revision is informational.</summary>
        uint32_t ActualVendorId = 0;
        uint32_t ActualProductCode = 0;
        uint32_t ActualRevision = 0;
        /// <summary>Digital input/output channel counts actually mapped — only populated for a
        /// digital I/O module result; always 0 for an axis result.</summary>
        int InputBitCount = 0;
        int OutputBitCount = 0;

        bool IsOk() const { return Status == AxisConnectStatus::Ok; }
        int Address() const { return AxisAddress::Make(SlavePosition, AxisChannel); }

        std::string ToString() const
        {
            return std::format("[{}] {}: {} - {}", AxisAddress::Text(Address()), AxisName, ecm::ToString(Status), Message);
        }
    };
}
