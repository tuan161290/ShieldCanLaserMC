#pragma once
#include <cstdint>
#include <format>
#include <optional>
#include <string>

#include "EtherCat/Models/EsiPdoInfo.h"

namespace ecm
{
    /// <summary>
    /// One expansion module a modular EtherCAT coupler can carry, as declared in the coupler's ESI
    /// &lt;Descriptions&gt;&lt;Modules&gt; section (e.g. the Leadshine R1EC's SC-3200-1 / SC-0032-N
    /// terminals). A module is NOT an EtherCAT slave of its own — it is identified by ModuleIdent,
    /// which the coupler reports at runtime through object 0xF050 "Detected Module Ident List".
    /// </summary>
    struct EsiModuleInfo
    {
        /// <summary>Module ident (ESI &lt;Type ModuleIdent="#x71101045"&gt;).</summary>
        uint32_t ModuleIdent = 0;
        /// <summary>Module type name from the ESI &lt;Type&gt; element text, e.g. "SC-3200-1".</summary>
        std::string TypeName;
        /// <summary>Friendly name (&lt;Name&gt;). Falls back to TypeName when the ESI declares no name.</summary>
        std::string DisplayName;
        /// <summary>ESI ModuleClass attribute — "Digital In", "Digital Out", "Analog In", ...</summary>
        std::string ModuleClass;
        /// <summary>Product code of the coupler whose ESI declared this module.</summary>
        uint32_t CouplerProductCode = 0;

        /// <summary>Total input bits this module contributes to the coupler's process image, summed
        /// from its TxPdo entries (e.g. 32 for an SC-3200-1). 0 for an output-only module. Has to
        /// come from the ESI: a coupler like the R1EC serves no per-slot PDO mapping object, yet
        /// the master must program the SyncManager lengths accordingly.</summary>
        int InputBits = 0;
        /// <summary>Total output bits, summed from its RxPdo entries. 0 for an input-only module.</summary>
        int OutputBits = 0;

        /// <summary>The TxPdo this module contributes. nullopt for an output-only module.</summary>
        std::optional<EsiPdoInfo> InputPdo;
        /// <summary>The RxPdo this module contributes. nullopt for an input-only module.</summary>
        std::optional<EsiPdoInfo> OutputPdo;
        /// <summary>The mutually exclusive alternative to InputPdo — modules declare the same
        /// channels twice (one entry per bit, and packed into words); only one may ever be assigned,
        /// and a device may accept the mapping download for only one of them.</summary>
        std::optional<EsiPdoInfo> InputPdoAlt;
        /// <summary>Alternative to OutputPdo; see InputPdoAlt.</summary>
        std::optional<EsiPdoInfo> OutputPdoAlt;

        std::string ToString() const
        {
            return std::format("0x{:08X} {}", ModuleIdent, DisplayName) + (ModuleClass.empty() ? "" : std::format(" [{}]", ModuleClass));
        }
    };
}
