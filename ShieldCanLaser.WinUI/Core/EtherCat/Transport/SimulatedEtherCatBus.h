#pragma once
#include <chrono>
#include <map>
#include <mutex>
#include <set>

#include "EtherCat/Constants/CiA402Registers.h"
#include "EtherCat/Transport/IEtherCatBus.h"

namespace ecm
{
    /// <summary>
    /// In-memory simulated EtherCAT bus. Implements the CiA402 state machine and Profile Position
    /// (pp) mode motion closely enough to build/test the UI and EtherCatMasterService without real
    /// hardware. Not a substitute for validating against actual drives before commissioning.
    /// Mọi hàm đều khoá một mutex nội bộ nên gọi getter/setter từ luồng khác trong lúc
    /// CyclicExchange() chạy trên luồng polling là an toàn.
    /// </summary>
    class SimulatedEtherCatBus : public IEtherCatBus
    {
    public:
        SimulatedEtherCatBus() = default;
        ~SimulatedEtherCatBus() override;

        // ── Test-only hooks (C#: các collection công khai SimulateInputs / SimulateMissingAt /
        // SimulatedPresence; ở đây truy cập qua hàm để an toàn luồng) ──

        /// <summary>Live digital input bits a simulated I/O module at this SlavePosition should
        /// report, channel N in bit N. Not present = reads back as 0.</summary>
        void SetSimulateInputs(int slavePosition, uint64_t bits);
        void RemoveSimulateInputs(int slavePosition);
        std::map<int, uint64_t> SimulateInputs() const;

        /// <summary>Slave positions listed here are reported as "no response" by ConnectAllAxis —
        /// lets a config UI be exercised against the ⚠ NoResponse path. Empty by default.</summary>
        void SetSimulateMissingAt(int slavePosition, bool missing);
        std::set<int> SimulateMissingAt() const;

        /// <summary>One module of a simulated modular I/O slave (coupler + modules), in slot order.</summary>
        struct SimIoSlot
        {
            int InputBits = 0;
            int OutputBits = 0;
        };

        /// <summary>Rack shape every simulated I/O slave reports: one entry per module, channels concatenated in
        /// slot order (inputs of slot 0 are DI 0…, then slot 1…; same for outputs). Default {{8, 8}} — the C#
        /// simulator's fixed one-module 8 DI / 8 DO. At most 32 bits per direction per module (one IORegister) and 64
        /// in total. Set before ConnectAllAxis.</summary>
        void SetSimulatedIoLayout(std::vector<SimIoSlot> slots);

        /// <summary>What ScanSlaves finds on this simulated bus. Empty by default.</summary>
        void SetSimulatedPresence(int slavePosition, const DiscoveredSlaveInfo& info);
        void RemoveSimulatedPresence(int slavePosition);
        std::map<int, DiscoveredSlaveInfo> SimulatedPresence() const;

        // ── IEtherCatBus ──
        bool Open(const std::string& networkAdapter, int expectedSlaveCount) override;
        std::vector<AxisConnectResult> ConnectAllAxis(const std::string& networkAdapter,
                                                      const std::vector<AxisConfig>& axes,
                                                      const std::vector<IoModuleConfig>& ioModules,
                                                      std::vector<AxisConnectResult>& ioResults) override;
        std::vector<DiscoveredSlaveInfo> ScanSlaves(const std::string& networkAdapter) override;
        void Close() override;
        int SlaveCount() const override;
        void CyclicExchange() override;

        void WriteSdo(int slavePosition, uint16_t index, uint8_t subIndex, int value, uint8_t sizeBytes) override;
        int ReadSdo(int slavePosition, uint16_t index, uint8_t subIndex, uint8_t sizeBytes) override;
        bool TryReadSdo(int slavePosition, uint16_t index, uint8_t subIndex, uint8_t sizeBytes, int& value) override;

        void SetControlword(int slavePosition, uint16_t controlword) override;
        void SetModeOfOperation(int slavePosition, int8_t mode) override;
        void SetTargetPosition(int slavePosition, int counts) override;
        void SetProfileVelocity(int slavePosition, uint32_t countsPerSec) override;
        void SetProfileAcceleration(int slavePosition, uint32_t countsPerSec2) override;
        void SetProfileDeceleration(int slavePosition, uint32_t countsPerSec2) override;

        uint16_t GetStatusword(int slavePosition) override;
        int8_t GetModeOfOperationDisplay(int slavePosition) override;
        int GetActualPosition(int slavePosition) override;
        int GetActualVelocity(int slavePosition) override;
        int16_t GetActualTorque(int slavePosition) override;
        uint16_t GetErrorCode(int slavePosition) override;

        uint64_t GetDigitalInputs(int slavePosition) override;
        void SetDigitalOutputs(int slavePosition, uint64_t mask) override;
        uint32_t GetIoModuleDeviceStatus(int slavePosition) override;
        std::vector<uint32_t> GetDetectedModuleIdents(int slavePosition) override;

        int GetIoSlotCount(int slavePosition) override;
        uint32_t GetIoSlotIdent(int slavePosition, int slot) override;
        int GetIoSlotInputBitCount(int slavePosition, int slot) override;
        int GetIoSlotOutputBitCount(int slavePosition, int slot) override;
        uint32_t GetIoSlotInputRegister(int slavePosition, int slot) override;
        void SetIoSlotOutputRegister(int slavePosition, int slot, uint32_t value) override;

        CyclicStats GetCyclicStats() override;
        void SetModuleCatalog(const std::vector<EsiModuleInfo>& modules) override;
        void SetSyncManagerCatalog(const std::vector<EsiSyncManagerInfo>& syncManagers) override;

        /// <summary>Không có trong interface C#: mặt nạ DO lần ghi gần nhất của module I/O mô phỏng
        /// (0 nếu không có module ở vị trí đó) — để kiểm thử/giám sát đọc lại.</summary>
        uint64_t GetSimulatedOutputs(int slavePosition) const;

    private:
        struct SlaveSim
        {
            uint16_t Controlword = 0;
            uint16_t Statusword = CiA402Registers::Statusword::SwitchOnDisabled;
            int8_t Mode = 0;
            int TargetPosition = 0;
            uint32_t ProfileVelocity = 1;
            uint32_t ProfileAcceleration = 1;
            uint32_t ProfileDeceleration = 1;
            double ActualPosition = 0;
            double ActualVelocity = 0;
        };

        struct IoSim
        {
            uint64_t Outputs = 0;
        };

        static void StepSlave(SlaveSim& s, double dt);
        SlaveSim& Get(int slavePosition);
        IoSim& GetIo(int slavePosition);
        void CloseLocked();
        void RestartClockLocked();
        double ElapsedSecondsLocked() const;

        mutable std::mutex m_mutex;
        std::map<int, SlaveSim> m_slaves;
        std::map<int, IoSim> m_ioSlaves;
        std::map<int, uint64_t> m_simulateInputs;
        std::vector<SimIoSlot> m_ioLayout{ SimIoSlot{ 8, 8 } };
        std::set<int> m_simulateMissingAt;
        std::map<int, DiscoveredSlaveInfo> m_simulatedPresence;
        bool m_clockRunning = false;   // Stopwatch: Restart() lúc mở, Reset() lúc đóng
        std::chrono::steady_clock::time_point m_clockStart{};
        double m_lastCycleSeconds = 0;
        int m_slaveCount = 0;
    };
}
