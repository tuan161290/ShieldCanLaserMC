#pragma once
#include <string>
#include <vector>

#include "EtherCat/Models/EsiDeviceInfo.h"
#include "EtherCat/Models/EsiModuleInfo.h"
#include "EtherCat/Models/EsiSyncManagerInfo.h"
#include "EtherCat/Models/EsiVendorInfo.h"

namespace ecm
{
    /// <summary>
    /// Loads the driver-type catalog from ESI XML files dropped into a folder — lets the config UI
    /// populate an axis slot's expected driver type before any hardware is connected (Vendor -> its
    /// supported Drives). Only identity fields are parsed for devices; PDO/object-dictionary mapping
    /// of drives is intentionally NOT read from ESI — that stays live (DiscoverPdoLayout), since a
    /// drive's actual PDO assignment is more trustworthy than the ESI's declared default.
    /// </summary>
    class EsiCatalog
    {
    public:
        /// <summary>Parse every *.xml file under folderPath (recursing into subfolders) and return
        /// one entry per declared device. Files that fail to parse are skipped rather than thrown —
        /// one bad/foreign XML file should not block the rest of the catalog from loading.</summary>
        static std::vector<EsiDeviceInfo> LoadFromFolder(const std::string& folderPath);

        /// <summary>Same as LoadFromFolder, but grouped by vendor — a vendor that ships several ESI
        /// files still collapses into one EsiVendorInfo with a combined Drives list.</summary>
        static std::vector<EsiVendorInfo> LoadVendorsFromFolder(const std::string& folderPath);

        /// <summary>Parse the &lt;Descriptions&gt;&lt;Modules&gt; section of every ESI under folderPath
        /// — the expansion terminals a modular coupler can carry. Gives a name (and bit widths) to
        /// the raw idents a coupler reports at runtime in 0xF050.</summary>
        static std::vector<EsiModuleInfo> LoadModulesFromFolder(const std::string& folderPath);

        /// <summary>Parse every device's declared SyncManagers. Only needed for slaves whose EEPROM
        /// does not carry an SII SyncManager category for the process-data SMs (the R1EC).</summary>
        static std::vector<EsiSyncManagerInfo> LoadSyncManagersFromFolder(const std::string& folderPath);
    };
}
