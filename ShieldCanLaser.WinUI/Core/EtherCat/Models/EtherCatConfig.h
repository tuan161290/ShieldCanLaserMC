#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "EtherCat/Models/AxisAddress.h"
#include "EtherCat/Models/AxisUnit.h"
#include "EtherCat/Models/EsiModuleInfo.h"
#include "EtherCat/Models/EsiSyncManagerInfo.h"
#include "EtherCat/Models/IoModuleConfig.h"
#include "EtherCat/Models/SpeedUnit.h"

namespace ecm
{
    /// <summary>Per-axis configuration.</summary>
    struct AxisConfig
    {
        /// <summary>Display name for this axis (e.g. "X_AXIS", "Y_AXIS")</summary>
        std::string Name = "Axis 1";
        /// <summary>EtherCAT slave position on the bus (0-based, daisy-chain order).</summary>
        int SlavePosition = 0;
        /// <summary>Kênh trục trong slave: 0 với driver một trục; driver hai trục (Welcon WE2x) có trục 2 ở kênh 1 —
        /// đối tượng 0x6000+ dời 0x800 (xem AxisAddress).</summary>
        int AxisChannel = 0;
        /// <summary>Địa chỉ trục truyền cho mọi API trục của bus/service (= SlavePosition khi kênh 0).</summary>
        int Address() const { return AxisAddress::Make(SlavePosition, AxisChannel); }
        /// <summary>Scaling: raw drive position counts per 1 user unit (e.g. encoder counts per mm).</summary>
        double CountsPerUnit = 1000.0;
        /// <summary>Homing method (CiA402 object 0x6098). Drive/vendor specific.</summary>
        int8_t HomingMethod = 1;
        /// <summary>Homing speed (units/s), written to 0x6099:1/:2 before every StartHome. Kept
        /// separate from DefaultVelocity so homing doesn't silently run at whatever profile velocity
        /// a previous move left behind — defaults slower since homing runs toward a limit switch.</summary>
        double HomingVelocity = 10.0;
        /// <summary>Default profile velocity (units/s) used when not explicitly specified.</summary>
        double DefaultVelocity = 50.0;
        /// <summary>Default acceleration/deceleration time (s) used when not explicitly specified.</summary>
        double DefaultAccelTimeS = 0.2;
        double DefaultDecelTimeS = 0.2;

        // ── Real drive parameters (SDO) — written to the actual drive by ConnectAllAxis once
        // validation succeeds. nullopt = leave the drive's current value alone.

        /// <summary>Software position limit, negative direction (axis units) — written to 0x607D:1.</summary>
        std::optional<double> SoftwareLimitNegativeUnits;
        /// <summary>Software position limit, positive direction (axis units) — written to 0x607D:2.</summary>
        std::optional<double> SoftwareLimitPositiveUnits;
        /// <summary>Physical unit CountsPerUnit/TravelPerRotationUnits are expressed in — descriptive.</summary>
        AxisUnit Unit = AxisUnit::mm;
        /// <summary>Engineering unit velocities are expressed in (see SpeedUnit).</summary>
        ecm::SpeedUnit SpeedUnit = ecm::SpeedUnit::UnitPerSec;
        /// <summary>Distance travelled per one motor revolution, in Unit — written to CiA402 0x6092:1
        /// "Feed constant / Feed increments" via CountsPerUnit.</summary>
        std::optional<double> TravelPerRotationUnits;

        // ── Expected driver identity (from the ESI catalog selection) — compared against the slave
        // actually found at SlavePosition when ConnectAllAxis validates the config.
        uint32_t ExpectedVendorId = 0;
        uint32_t ExpectedProductCode = 0;
        /// <summary>Not enforced by ConnectAllAxis (firmware revision drift is common) — display only.</summary>
        uint32_t ExpectedRevision = 0;
        /// <summary>Display name of the assigned driver — not used for identity matching.</summary>
        std::string ExpectedDriverName;

        /// <summary>Whether this slot has a driver assigned. A slot with ExpectedProductCode == 0 is
        /// "Unconfigured" and is skipped by ConnectAllAxis.</summary>
        bool IsConfigured() const { return ExpectedProductCode != 0; }
    };

    /// <summary>EtherCAT bus and multi-axis configuration.</summary>
    struct EtherCatConfig
    {
        /// <summary>Network adapter the master binds to (e.g. Npcap device name on Windows).
        /// Not used by SimulatedEtherCatBus.</summary>
        std::string NetworkAdapter;
        /// <summary>Cyclic PDO exchange period, in milliseconds.</summary>
        int CycleTimeMs = 4;
        /// <summary>Position tolerance (axis units) for "in position" checks.</summary>
        double PositionTolerance = 0.05;
        /// <summary>Duration (ms) to hold the pp-mode "New set-point" controlword bit before clearing it.</summary>
        int NewSetpointPulseMs = 20;
        /// <summary>Configured axes on this EtherCAT bus, in slave position order.</summary>
        std::vector<AxisConfig> Axes;
        /// <summary>Expansion modules known from the loaded ESI files, used to size a modular
        /// coupler's SyncManagers (see IEtherCatBus::SetModuleCatalog). Fill from
        /// EsiCatalog::LoadModulesFromFolder. Not persisted ([JsonIgnore]) — derived from the ESI folder.</summary>
        std::vector<EsiModuleInfo> ModuleCatalog;
        /// <summary>SyncManagers each ESI device declares, needed for slaves whose EEPROM has no SII
        /// SyncManager category for the process-data SMs (the R1EC). Not persisted ([JsonIgnore]).</summary>
        std::vector<EsiSyncManagerInfo> SyncManagerCatalog;
        /// <summary>Configured digital I/O remote modules on this bus, in slave position order.</summary>
        std::vector<IoModuleConfig> IoModules;
    };
}
