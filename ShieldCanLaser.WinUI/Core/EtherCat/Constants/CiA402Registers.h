#pragma once
#include <cstdint>
#include <string>

namespace ecm
{
    /// <summary>
    /// CiA 402 (CANopen/EtherCAT drive profile) object dictionary map and Controlword/Statusword
    /// bit definitions. Standardized across virtually all EtherCAT servo drives (Beckhoff,
    /// Panasonic, Delta, Yaskawa, ...).
    /// </summary>
    struct CiA402Registers
    {
        // ── Control / Status ──
        static constexpr uint16_t CONTROLWORD = 0x6040;            // R/W, UINT16
        static constexpr uint16_t STATUSWORD = 0x6041;             // RO, UINT16
        static constexpr uint16_t ERROR_CODE = 0x603F;             // RO, UINT16
        static constexpr uint16_t SUPPORTED_DRIVE_MODES = 0x6502;  // RO, UINT32 bitmask

        // ── Modes of Operation ──
        static constexpr uint16_t MODES_OF_OPERATION = 0x6060;          // R/W, INT8
        static constexpr uint16_t MODES_OF_OPERATION_DISPLAY = 0x6061;  // RO, INT8

        // ── Profile Position (pp) mode ──
        static constexpr uint16_t TARGET_POSITION = 0x607A;       // R/W, INT32 (pp/csp)
        static constexpr uint16_t PROFILE_VELOCITY = 0x6081;      // R/W, UINT32 (pp)
        static constexpr uint16_t PROFILE_ACCELERATION = 0x6083;  // R/W, UINT32 (pp/pv)
        static constexpr uint16_t PROFILE_DECELERATION = 0x6084;  // R/W, UINT32 (pp/pv)

        // ── Profile Velocity (pv) / Cyclic Sync Velocity (csv) mode ──
        static constexpr uint16_t TARGET_VELOCITY = 0x60FF;        // R/W, INT32 (pv/csv)
        static constexpr uint16_t VELOCITY_ACTUAL_VALUE = 0x606C;  // RO, INT32

        // ── Profile Torque (pt) / Cyclic Sync Torque (cst) mode — dành cho giai đoạn sau ──
        static constexpr uint16_t TARGET_TORQUE = 0x6071;        // R/W, INT16, 0.1% rated torque
        static constexpr uint16_t MAX_TORQUE = 0x6072;           // R/W, UINT16, 0.1% rated torque
        static constexpr uint16_t TORQUE_ACTUAL_VALUE = 0x6077;  // RO, INT16, 0.1% rated torque

        // ── Position / Homing ──
        static constexpr uint16_t POSITION_ACTUAL_VALUE = 0x6064;         // RO, INT32
        static constexpr uint16_t FOLLOWING_ERROR_ACTUAL_VALUE = 0x60F4;  // RO, INT32
        static constexpr uint16_t HOMING_METHOD = 0x6098;                 // R/W, INT8
        /// <summary>Homing speeds (sub1=switch search, sub2=zero search) — R/W, UINT32</summary>
        static constexpr uint16_t HOMING_SPEED = 0x6099;
        static constexpr uint16_t HOMING_ACCELERATION = 0x609A;           // R/W, UINT32
        static constexpr uint16_t HOME_OFFSET = 0x607C;                   // R/W, INT32
        /// <summary>Software position limit — R/W, INT32 (sub1 = min/negative, sub2 = max/positive).
        /// Confirmed present on the Leadshine CL3B-EC507 ESI — verified writable via SDO against
        /// real hardware.</summary>
        static constexpr uint16_t SOFTWARE_POSITION_LIMIT = 0x607D;
        /// <summary>Position encoder resolution — RO, UDINT×2 (sub1 = Encoder increments, sub2 =
        /// Motor revolutions). Read-only on the CL3B-EC507 — read during Scan for display only.</summary>
        static constexpr uint16_t POSITION_ENCODER_RESOLUTION = 0x608F;
        /// <summary>Feed constant — sub1 "Feed increments" R/W, sub2 "Shaft revolutions" RO, both
        /// UDINT. Only sub1 is writable on the CL3B-EC507.</summary>
        static constexpr uint16_t FEED_CONSTANT = 0x6092;

        /// <summary>Controlword bits (object 0x6040)</summary>
        struct Controlword
        {
            static constexpr uint16_t SwitchOn = 1 << 0;
            static constexpr uint16_t EnableVoltage = 1 << 1;
            static constexpr uint16_t QuickStop = 1 << 2;             // active low: 0 = quick stop
            static constexpr uint16_t EnableOperation = 1 << 3;
            static constexpr uint16_t NewSetPoint = 1 << 4;           // pp mode
            static constexpr uint16_t ChangeSetImmediately = 1 << 5;  // pp mode
            static constexpr uint16_t AbsRel = 1 << 6;                // pp mode: 0=absolute, 1=relative
            static constexpr uint16_t FaultReset = 1 << 7;            // rising edge clears fault
            static constexpr uint16_t Halt = 1 << 8;
            static constexpr uint16_t HomingStart = 1 << 4;           // homing mode: alias of bit4
        };

        /// <summary>Standard controlword command values for CiA402 state machine transitions.</summary>
        struct ControlwordCmd
        {
            /// <summary>"Switch on disabled" -> "Ready to switch on"</summary>
            static constexpr uint16_t Shutdown = Controlword::EnableVoltage | Controlword::QuickStop;
            /// <summary>"Ready to switch on" -> "Switched on"</summary>
            static constexpr uint16_t SwitchOn = Shutdown | Controlword::SwitchOn;
            /// <summary>"Switched on" -> "Operation enabled"</summary>
            static constexpr uint16_t EnableOperation = SwitchOn | Controlword::EnableOperation;
            /// <summary>Any state -> "Switch on disabled"</summary>
            static constexpr uint16_t DisableVoltage = 0x0000;
            /// <summary>Any state -> "Quick stop active"</summary>
            static constexpr uint16_t QuickStop = Controlword::EnableVoltage;
            /// <summary>Fault -> "Switch on disabled" (rising edge clears fault)</summary>
            static constexpr uint16_t FaultReset = Controlword::FaultReset;
        };

        /// <summary>Statusword bits (object 0x6041)</summary>
        struct Statusword
        {
            static constexpr uint16_t ReadyToSwitchOn = 1 << 0;
            static constexpr uint16_t SwitchedOn = 1 << 1;
            static constexpr uint16_t OperationEnabled = 1 << 2;
            static constexpr uint16_t Fault = 1 << 3;
            static constexpr uint16_t VoltageEnabled = 1 << 4;
            static constexpr uint16_t QuickStop = 1 << 5;   // active low: 0 = quick stop active
            static constexpr uint16_t SwitchOnDisabled = 1 << 6;
            static constexpr uint16_t Warning = 1 << 7;
            static constexpr uint16_t Remote = 1 << 9;
            static constexpr uint16_t TargetReached = 1 << 10;
            static constexpr uint16_t InternalLimitActive = 1 << 11;
            /// <summary>Mode-specific: Set-point Acknowledge (pp mode) / Homing Attained (homing mode)</summary>
            static constexpr uint16_t SetpointAckOrHomingAttained = 1 << 12;
            /// <summary>Mode-specific: Following Error (pp/csp mode) / Homing Error (homing mode)</summary>
            static constexpr uint16_t FollowingErrorOrHomingError = 1 << 13;
        };

        /// <summary>Decode the CiA402 device state from a raw Statusword value (for diagnostics/logging).</summary>
        static std::string DecodeState(uint16_t statusword);
    };

    /// <summary>Modes of Operation values (objects 0x6060 / 0x6061).</summary>
    enum class OperationMode : int8_t
    {
        None = 0,
        ProfilePosition = 1,        // pp
        ProfileVelocity = 3,        // pv
        ProfileTorque = 4,          // pt — implement khi thêm Torque mode
        Homing = 6,                 // hm
        InterpolatedPosition = 7,   // ip
        CyclicSyncPosition = 8,     // csp
        CyclicSyncVelocity = 9,     // csv
        CyclicSyncTorque = 10       // cst — implement khi thêm Torque mode
    };

    /// <summary>Tên thành viên enum (như ToString() của C#); "" nếu giá trị không có tên.</summary>
    const char* ToString(OperationMode mode);
}
