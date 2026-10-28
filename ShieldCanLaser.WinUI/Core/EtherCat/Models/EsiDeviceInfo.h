#pragma once
#include <cstdint>
#include <format>
#include <string>

namespace ecm
{
    /// <summary>
    /// Identity of one EtherCAT device type declared in an ESI XML file — populates the driver-type
    /// catalog so a user can pick a driver for an axis slot before any hardware is connected. One
    /// ESI file can declare several device types — each becomes one EsiDeviceInfo.
    /// </summary>
    struct EsiDeviceInfo
    {
        /// <summary>EtherCAT Vendor ID (from &lt;Vendor&gt;&lt;Id&gt;), e.g. 17185 for Leadshine.</summary>
        uint32_t VendorId = 0;
        /// <summary>Vendor display name (from &lt;Vendor&gt;&lt;Name&gt;).</summary>
        std::string VendorName;
        /// <summary>Product code (from &lt;Type ProductCode="..."&gt;).</summary>
        uint32_t ProductCode = 0;
        /// <summary>Revision number (from &lt;Type RevisionNo="..."&gt;).</summary>
        uint32_t RevisionNo = 0;
        /// <summary>Short model/type name — the &lt;Type&gt; element's inner text, e.g. "CL3B-EC507".</summary>
        std::string TypeName;
        /// <summary>Friendly display name (from &lt;Device&gt;&lt;Name&gt;), e.g. "L5N(COE)".</summary>
        std::string DisplayName;
        /// <summary>Device category (from &lt;Device&gt;&lt;GroupType&gt;), e.g. "Servo Drive" — used to
        /// tell a CiA402 servo axis apart from a digital I/O remote module.</summary>
        std::string GroupType;
        /// <summary>ESI file (name only) this entry was parsed from, for diagnostics.</summary>
        std::string SourceFile;

        std::string ToString() const
        {
            return std::format("{} (Vendor 0x{:X}, Product 0x{:X}, Rev {})", DisplayName, VendorId, ProductCode, RevisionNo);
        }
    };
}
