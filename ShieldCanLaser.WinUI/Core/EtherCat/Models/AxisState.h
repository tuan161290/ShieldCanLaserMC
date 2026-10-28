#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace ecm
{
    /// <summary>
    /// Per-axis runtime state, decoded from CiA402 Statusword (0x6041) each cycle by
    /// EtherCatMasterService. Luồng polling ghi, các luồng khác đọc → mỗi trường vô hướng là
    /// std::atomic; đối tượng không copy được và được chia sẻ qua std::shared_ptr. Name/SlavePosition
    /// được gán trước khi chia sẻ và không đổi về sau.
    /// </summary>
    class AxisState
    {
    public:
        AxisState() = default;
        AxisState(const AxisState&) = delete;
        AxisState& operator=(const AxisState&) = delete;

        // ── Config ──
        /// <summary>Display name (from AxisConfig)</summary>
        std::string Name;
        /// <summary>EtherCAT slave position (from AxisConfig)</summary>
        int SlavePosition = 0;
        /// <summary>Địa chỉ trục (slave + kênh, AxisConfig::Address) — khoá tra cứu của GetAxisState.</summary>
        int Address = 0;

        // ── Statusword (0x6041) bits ──
        std::atomic<bool> ReadyToSwitchOn{ false };
        std::atomic<bool> SwitchedOn{ false };
        /// <summary>Operation enabled — axis is actively servoing</summary>
        std::atomic<bool> OperationEnabled{ false };
        std::atomic<bool> Fault{ false };
        std::atomic<bool> VoltageEnabled{ false };
        std::atomic<bool> QuickStopActive{ false };
        std::atomic<bool> SwitchOnDisabled{ false };
        /// <summary>Warning active (non-fatal)</summary>
        std::atomic<bool> Warning{ false };
        /// <summary>Axis has reached its commanded target (pp/csp mode)</summary>
        std::atomic<bool> TargetReached{ false };
        /// <summary>An internal limit (position/velocity/torque) is active</summary>
        std::atomic<bool> InternalLimitActive{ false };
        /// <summary>Mode-specific bit 12: Set-point Acknowledge (pp) / Homing Attained (homing)</summary>
        std::atomic<bool> SetpointAckOrHomingAttained{ false };
        /// <summary>Mode-specific bit 13: Following Error (pp/csp) / Homing Error (homing)</summary>
        std::atomic<bool> FollowingErrorOrHomingError{ false };

        /// <summary>Raw Statusword value, kept for diagnostics/advanced use.</summary>
        std::atomic<uint16_t> Statusword{ 0 };
        /// <summary>Modes of operation display (0x6061)</summary>
        std::atomic<int8_t> ModeOfOperationDisplay{ 0 };

        // ── Position / Velocity / Torque ──
        /// <summary>Actual position, raw drive counts (0x6064)</summary>
        std::atomic<int> ActualPositionRaw{ 0 };
        /// <summary>Actual position, scaled to axis units via AxisConfig.CountsPerUnit</summary>
        std::atomic<double> ActualPositionUnits{ 0.0 };
        /// <summary>Actual velocity, raw counts/s (0x606C)</summary>
        std::atomic<int> ActualVelocityRaw{ 0 };
        /// <summary>Actual torque, 0.1% rated torque (0x6077)</summary>
        std::atomic<int16_t> ActualTorque{ 0 };
        /// <summary>Error code (0x603F). 0 = no error.</summary>
        std::atomic<uint16_t> ErrorCode{ 0 };

        // ── Derived State ──
        /// <summary>Communication with this axis is active</summary>
        std::atomic<bool> IsConnected{ false };
        /// <summary>Emergency stop active (set by consumer application)</summary>
        std::atomic<bool> IsEMG{ false };

        /// <summary>Ready to accept a new motion command.</summary>
        bool CanMove() const { return OperationEnabled.load() && !Fault.load() && !IsEMG.load(); }
    };
}
