#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "EtherCat/Models/AxisConnectResult.h"
#include "EtherCat/Models/CyclicStats.h"
#include "EtherCat/Models/DiscoveredSlaveInfo.h"
#include "EtherCat/Models/EsiModuleInfo.h"
#include "EtherCat/Models/EsiSyncManagerInfo.h"
#include "EtherCat/Models/EtherCatConfig.h"

namespace ecm
{
    /// <summary>
    /// Abstraction over the actual EtherCAT network transport. EtherCatMasterService talks only to
    /// this interface — SimulatedEtherCatBus or the SOEM-backed implementation. Implementations call
    /// Close() in their destructor (IDisposable của bản C#).
    /// </summary>
    class IEtherCatBus
    {
    public:
        virtual ~IEtherCatBus() = default;

        /// <summary>Open the bus on the given network adapter and expect this many slaves.</summary>
        virtual bool Open(const std::string& networkAdapter, int expectedSlaveCount) = 0;

        /// <summary>
        /// Validate a whole config in one call ("Connect All Axis"): for each axes entry (already
        /// filtered to IsConfigured by the caller) check that a slave exists at its SlavePosition
        /// and that its VendorId/ProductCode match the slot's Expected* fields, gathering a result
        /// for every slot instead of throwing at the first bad one. Same treatment for ioModules,
        /// returned separately via ioResults. Only brings the bus to EtherCAT OPERATIONAL if every
        /// slot (axis AND I/O) is Ok; otherwise the bus is left closed and every affected slot's
        /// status explains why.
        /// </summary>
        virtual std::vector<AxisConnectResult> ConnectAllAxis(const std::string& networkAdapter,
                                                              const std::vector<AxisConfig>& axes,
                                                              const std::vector<IoModuleConfig>& ioModules,
                                                              std::vector<AxisConnectResult>& ioResults) = 0;

        /// <summary>Discovery-only: open the bus just far enough to read every slave's identity,
        /// then close it again. Never leaves the bus open/connected.</summary>
        virtual std::vector<DiscoveredSlaveInfo> ScanSlaves(const std::string& networkAdapter) = 0;

        /// <summary>Close the bus.</summary>
        virtual void Close() = 0;

        /// <summary>Number of slaves actually found on the bus after Open().</summary>
        virtual int SlaveCount() const = 0;

        /// <summary>Perform one cyclic PDO exchange for all slaves. Deliberate no-op on the real bus
        /// (its frame exchange runs on a native real-time thread); the simulator needs it to step.</summary>
        virtual void CyclicExchange() = 0;

        // Các hàm TRỤC dưới đây (SDO và PDO CiA402) nhận địa chỉ trục AxisAddress — vị trí slave + kênh trục
        // trong slave. Với kênh > 0 bus tự dời chỉ số profile 0x6000–0x67FF (AxisAddress::ProfileIndex); vị trí
        // slave trần (kênh 0) vẫn hợp lệ, nên module I/O và driver một trục không đổi gì.

        // ── Acyclic (SDO) access — configuration, not for the hot loop ──
        // sizeBytes must match the object's actual wire size (1/2/4) — an expedited SDO transfer
        // with the wrong size is rejected (or misinterpreted) by most drives.
        virtual void WriteSdo(int slavePosition, uint16_t index, uint8_t subIndex, int value, uint8_t sizeBytes) = 0;
        virtual int ReadSdo(int slavePosition, uint16_t index, uint8_t subIndex, uint8_t sizeBytes) = 0;

        /// <summary>Like ReadSdo but reports whether the slave actually answered (ReadSdo returns 0
        /// both for "the value is zero" and for "the read failed"). False when the working counter
        /// came back 0.</summary>
        virtual bool TryReadSdo(int slavePosition, uint16_t index, uint8_t subIndex, uint8_t sizeBytes, int& value) = 0;

        // ── Cyclic PDO accessors — outputs (master -> slave) ──
        virtual void SetControlword(int slavePosition, uint16_t controlword) = 0;
        virtual void SetModeOfOperation(int slavePosition, int8_t mode) = 0;
        virtual void SetTargetPosition(int slavePosition, int counts) = 0;
        virtual void SetProfileVelocity(int slavePosition, uint32_t countsPerSec) = 0;
        virtual void SetProfileAcceleration(int slavePosition, uint32_t countsPerSec2) = 0;
        virtual void SetProfileDeceleration(int slavePosition, uint32_t countsPerSec2) = 0;

        // ── Cyclic PDO accessors — inputs (slave -> master) ──
        virtual uint16_t GetStatusword(int slavePosition) = 0;
        virtual int8_t GetModeOfOperationDisplay(int slavePosition) = 0;
        virtual int GetActualPosition(int slavePosition) = 0;
        virtual int GetActualVelocity(int slavePosition) = 0;
        virtual int16_t GetActualTorque(int slavePosition) = 0;
        virtual uint16_t GetErrorCode(int slavePosition) = 0;

        // ── Digital I/O (Fastech Ezi-IO, Leadshine R1EC and similar remote modules) — plain PDO.
        // 64-bit: an R1EC with two SC-3200-1 modules maps 64 DI in one module.
        /// <summary>Live digital input bits for the module at slavePosition, channel N in bit N.</summary>
        virtual uint64_t GetDigitalInputs(int slavePosition) = 0;
        /// <summary>Write all digital output bits for the module in one call (channel N in bit N) —
        /// callers flipping a single channel read-modify-write their own tracked copy.</summary>
        virtual void SetDigitalOutputs(int slavePosition, uint64_t mask) = 0;

        /// <summary>Raw 0xF100:01 "Device status" diagnostic word of the I/O coupler — 0 if this
        /// slave doesn't map it. Kept separate from GetDigitalInputs on purpose so a coupler fault
        /// doesn't masquerade as a DI bit.</summary>
        virtual uint32_t GetIoModuleDeviceStatus(int slavePosition) = 0;

        /// <summary>Expansion modules the modular coupler reported in 0xF050 "Detected Module Ident
        /// List" during bring-up, in slot order. Empty for a non-modular slave, or before a
        /// successful connect. Module idents, NOT slave identities.</summary>
        virtual std::vector<uint32_t> GetDetectedModuleIdents(int slavePosition) = 0;

        // ── Per-module IORegister — one register per slot of a modular coupler. Slot indices line
        // up with GetDetectedModuleIdents' order. All come back 0/no-op for a non-modular slave or
        // before a successful connect. 32-bit — no I/O module maps more than 32 channels.
        /// <summary>Number of module slots the coupler reported (0xF050). 0 for a non-modular slave.</summary>
        virtual int GetIoSlotCount(int slavePosition) = 0;
        /// <summary>Module ident plugged into this slot.</summary>
        virtual uint32_t GetIoSlotIdent(int slavePosition, int slot) = 0;
        /// <summary>How many of this slot's own bits are digital inputs.</summary>
        virtual int GetIoSlotInputBitCount(int slavePosition, int slot) = 0;
        /// <summary>How many of this slot's own bits are digital outputs.</summary>
        virtual int GetIoSlotOutputBitCount(int slavePosition, int slot) = 0;
        /// <summary>Live digital input bits for just this one module slot, channel N in bit N.</summary>
        virtual uint32_t GetIoSlotInputRegister(int slavePosition, int slot) = 0;
        /// <summary>Write all digital output bits for just this one module slot in one call.</summary>
        virtual void SetIoSlotOutputRegister(int slavePosition, int slot, uint32_t value) = 0;

        /// <summary>Cyclic-timer diagnostics accumulated since the bus was last opened. The
        /// simulated bus always returns a zeroed instance.</summary>
        virtual CyclicStats GetCyclicStats() = 0;

        /// <summary>Supply the expansion modules an ESI declares, keyed by module ident, before
        /// connecting. A modular coupler reports which idents are in its rack (0xF050) but exposes
        /// no per-slot PDO mapping object, so the widths needed to program the SyncManager lengths
        /// come from here (EsiModuleInfo.InputBits/OutputBits). Call before ConnectAllAxis.</summary>
        virtual void SetModuleCatalog(const std::vector<EsiModuleInfo>& modules) = 0;

        /// <summary>Supply the SyncManager addresses an ESI declares, for slaves whose EEPROM does
        /// not (the R1EC declares just its two mailbox SMs there and otherwise answers SAFE-OP with
        /// AL status 0x1E). Call before ConnectAllAxis.</summary>
        virtual void SetSyncManagerCatalog(const std::vector<EsiSyncManagerInfo>& syncManagers) = 0;
    };
}
