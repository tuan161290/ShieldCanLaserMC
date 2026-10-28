#pragma once
#include <cstdint>

namespace ecm
{
    /// <summary>
    /// Identifiers and PDO-mapping notes extracted from the Leadshine ESI files under
    /// EtherCATMaster/ESI/Leadshine. Used by the SOEM-backed IEtherCatBus to recognize/configure
    /// these drives on the bus.
    /// </summary>
    struct LeadshineDrives
    {
        /// <summary>CANopen/EtherCAT Vendor ID (object 0x1018:1) — same for L5N, L3N, CL3B, R1EC.</summary>
        static constexpr uint32_t VENDOR_ID = 17185; // 0x4321

        /// <summary>L5N product code (0x1018:2) / revision (0x1018:3).</summary>
        static constexpr uint32_t L5N_PRODUCT_CODE = 0xF0;
        static constexpr uint32_t L5N_REVISION = 1;

        /// <summary>L3N product code (0x1018:2) / revision (0x1018:3).</summary>
        static constexpr uint32_t L3N_PRODUCT_CODE = 0xF8;
        static constexpr uint32_t L3N_REVISION = 1;

        /// <summary>CL3B-EC507 stepper driver — from ESI\Leadshine\CL3B-EC_V1.08 XML.xml.</summary>
        static constexpr uint32_t CL3B_EC507_PRODUCT_CODE = 0x0800;
        static constexpr uint32_t CL3B_EC507_REVISION = 0x0001;

        /// <summary>R1EC remote I/O coupler (expansion modules are read from the coupler at bring-up) — from
        /// ESI\Leadshine\R1EC\R1EC-v1.4.0.0.xml.</summary>
        static constexpr uint32_t R1EC_PRODUCT_CODE = 0x61400015;
        static constexpr uint32_t R1EC_REVISION = 0x22092010;

        // Default PDO mapping (identical Sm/RxPdo/TxPdo layout on the drives). Only the objects
        // below are in the DEFAULT active PDO (Sm2/Sm3); everything else must go through SDO unless
        // the PDO assignment (0x1C12/0x1C13) is reconfigured at slave setup.

        /// <summary>Default active RxPdo (Sm2, outputs, cyclic). Controlword + Target Position are
        /// the only two objects EtherCatMasterService writes every cycle for pp mode.</summary>
        static constexpr const char* DEFAULT_RXPDO_NOTE =
            "Sm2: 0x6040 Controlword, 0x607A Target Position, 0x60B8 Touch Probe Function";

        /// <summary>Default active TxPdo (Sm3, inputs, cyclic). Covers everything PollAxisState
        /// reads every cycle EXCEPT Actual Velocity (0x606C) and Actual Torque (0x6077) — a real
        /// bus must read those via SDO.</summary>
        static constexpr const char* DEFAULT_TXPDO_NOTE =
            "Sm3: 0x603F Error Code, 0x6041 Statusword, 0x6061 Mode Display, 0x6064 Position Actual, "
            "0x60B9 Touch Probe Status, 0x60BA Touch Probe Pos1, 0x60FD Digital Inputs";

        /// <summary>NOT in the default PDO on either drive — must be written via SDO (once per
        /// move, never streamed cyclically).</summary>
        static constexpr const char* SDO_ONLY_OBJECTS_NOTE =
            "0x6060 Modes of Operation, 0x6081 Profile Velocity, 0x6083 Profile Acceleration, 0x6084 Profile Deceleration";
    };
}
