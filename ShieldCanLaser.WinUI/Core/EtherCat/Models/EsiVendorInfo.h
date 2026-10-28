#pragma once
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "EtherCat/Models/EsiDeviceInfo.h"

namespace ecm
{
    /// <summary>
    /// One vendor node in the ESI driver-type catalog, as built by EsiCatalog::LoadVendorsFromFolder
    /// — the Vendor ID/Name plus every device declared across all ESI files found for that vendor
    /// (several files of one vendor are merged into one node).
    /// </summary>
    struct EsiVendorInfo
    {
        uint32_t VendorId = 0;
        std::string VendorName;
        /// <summary>Every device this vendor's ESI file(s) declare.</summary>
        std::vector<EsiDeviceInfo> Drives;

        std::string ToString() const
        {
            return std::format("{} (Vendor 0x{:X}) - {} drive(s)", VendorName, VendorId, Drives.size());
        }
    };
}
