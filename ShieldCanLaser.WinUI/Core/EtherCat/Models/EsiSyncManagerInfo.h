#pragma once
#include <cstdint>
#include <string>

namespace ecm
{
    /// <summary>
    /// One SyncManager as declared by a device's ESI (&lt;Sm StartAddress=".." ControlByte=".."
    /// Enable="1"&gt;), in declaration order — that order IS the SyncManager index.
    ///
    /// Normally the master never needs this (SOEM programs the SMs from the SII category). The
    /// Leadshine R1EC's EEPROM declares only the two mailbox SyncManagers, so SM2/SM3 stay at
    /// StartAddr 0, are never programmed, and the coupler refuses SAFE-OP with AL status 0x1E.
    /// Supplying the addresses from the ESI is the only remaining source.
    /// </summary>
    struct EsiSyncManagerInfo
    {
        /// <summary>Owning device's vendor id / product code, so a lookup can be scoped to the slave found.</summary>
        uint32_t VendorId = 0;
        uint32_t ProductCode = 0;
        /// <summary>SyncManager index — the position of this &lt;Sm&gt; among its siblings.</summary>
        int Index = 0;
        /// <summary>Physical start address in the ESC's DPRAM (e.g. 0x1800 outputs, 0x1C00 inputs).</summary>
        int StartAddress = 0;
        /// <summary>SM control register byte (direction + buffer mode), e.g. 0x64 outputs, 0x20 inputs.</summary>
        int ControlByte = 0;
        /// <summary>ESI Enable attribute; maps to the SM Activate register bit.</summary>
        bool Enable = false;
        /// <summary>Element text: "MBoxOut" / "MBoxIn" / "Outputs" / "Inputs". Diagnostic only.</summary>
        std::string Purpose;
    };
}
