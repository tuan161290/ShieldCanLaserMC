#pragma once
#include <vector>

namespace ecm
{
    /// <summary>One entry of a PDO's mapping, as the ESI declares it: which object/subindex supplies
    /// how many bits. Encoded on the wire as (Index &lt;&lt; 16) | (SubIndex &lt;&lt; 8) | BitLength.</summary>
    struct EsiPdoEntryInfo
    {
        int Index = 0;
        int SubIndex = 0;
        int BitLength = 0;
    };

    /// <summary>
    /// One PDO a module declares. On a coupler whose ESI says PdoUpload="false" (the Leadshine R1EC)
    /// the mapping cannot be read back over SDO, so the master must trust the ESI for sizing. A PDO
    /// the ESI marks Fixed="true" has its mapping baked into firmware and must NOT be written
    /// (download is rejected with SDO abort 0x06090011); for a Fixed one only the PDO *assignment*
    /// is the master's job.
    /// </summary>
    struct EsiPdoInfo
    {
        /// <summary>PDO object index for slot 0, e.g. 0x1A00. Slot N uses this plus N x SlotPdoIncrement.</summary>
        int Index = 0;
        /// <summary>SyncManager the ESI attaches this PDO to, or -1 when it declares none (the
        /// mutually-exclusive packed-word variants carry no Sm attribute).</summary>
        int SyncManager = -1;
        /// <summary>ESI Fixed="true"/"1" — the mapping is firmware-fixed and must not be written.</summary>
        bool IsFixed = false;
        std::vector<EsiPdoEntryInfo> Entries;

        /// <summary>Sum of every entry's BitLength.</summary>
        int TotalBits() const
        {
            int bits = 0;
            for (const auto& e : Entries)
                bits += e.BitLength;
            return bits;
        }
    };
}
