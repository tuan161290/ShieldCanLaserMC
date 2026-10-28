#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Base/Event.h"
#include "EtherCat/Constants/CiA402Registers.h"
#include "EtherCat/Models/AxisState.h"
#include "EtherCat/Models/IoModuleState.h"
#include "EtherCat/Transport/IEtherCatBus.h"

namespace ecm
{
    /// <summary>
    /// Core EtherCAT master service driving CiA402 servo axes in Profile Position (pp) mode.
    /// Talks only to IEtherCatBus — defaults to SimulatedEtherCatBus; pass the SOEM-backed bus for
    /// real hardware. Mọi thao tác (ServoOn, MoveAbsolute, …) là HÀM CHẶN — gọi từ luồng nền.
    ///
    /// <para>Tham số <c>slavePosition</c> của các hàm TRỤC là địa chỉ trục (<see cref="AxisAddress"/>, =
    /// AxisConfig::Address()): bằng đúng vị trí slave với driver một trục, mang thêm kênh với driver hai trục. Hàm
    /// I/O vẫn nhận vị trí slave thường.</para>
    /// </summary>
    class EtherCatMasterService
    {
    public:
        /// <summary>Create the service. bus == nullptr → an in-memory simulator.</summary>
        explicit EtherCatMasterService(std::shared_ptr<IEtherCatBus> bus = nullptr);
        ~EtherCatMasterService();
        EtherCatMasterService(const EtherCatMasterService&) = delete;
        EtherCatMasterService& operator=(const EtherCatMasterService&) = delete;

        // ── Properties ──
        /// <summary>Whether the EtherCAT bus is open and cyclic exchange is running.</summary>
        bool IsConnected() const { return m_isConnected.load(); }
        /// <summary>Runtime state for each configured axis. Vector chỉ được thay (hoán đổi nguyên
        /// khối, dưới khoá) bên trong Connect/ConnectAllAxis; tham chiếu trả về KHÔNG được duyệt
        /// đồng thời với một lần kết nối lại — khi cần an toàn tuyệt đối dùng AxesSnapshot() hoặc
        /// GetAxisState().</summary>
        const std::vector<std::shared_ptr<AxisState>>& Axes() const { return m_axes; }
        /// <summary>Runtime state for each configured digital I/O remote module (cùng lưu ý như Axes()).</summary>
        const std::vector<std::shared_ptr<IoModuleState>>& IoModules() const { return m_ioModules; }
        std::vector<std::shared_ptr<AxisState>> AxesSnapshot() const;
        std::vector<std::shared_ptr<IoModuleState>> IoModulesSnapshot() const;
        /// <summary>Current configuration — bản chụp bất biến của config truyền vào lần
        /// Connect/ConnectAllAxis gần nhất; nullptr trước lần đầu.</summary>
        std::shared_ptr<const EtherCatConfig> Config() const;
        std::shared_ptr<IEtherCatBus> Bus() const { return m_bus; }

        // ── Events (handler chạy trên luồng phát; OnAlarm/OnIoModuleFault và một phần OnLog phát
        // trên luồng polling khi đang giữ khoá bus — handler không được gọi ngược vào service) ──
        /// <summary>Raised when a log message is generated ("[HH:mm:ss.fff] message").</summary>
        base::Event<std::string> OnLog;
        /// <summary>Raised when a new fault is detected. Args: (slavePosition, errorCode)</summary>
        base::Event<int, uint16_t> OnAlarm;
        /// <summary>Raised when an I/O coupler's 0xF100:01 Device status changes to a non-zero
        /// value. Args: (slavePosition, deviceStatus).</summary>
        base::Event<int, uint32_t> OnIoModuleFault;
        /// <summary>Raised when connection state changes.</summary>
        base::Event<bool> OnConnectionChanged;

        // ── Connection ──
        /// <summary>Open the EtherCAT bus, initialize axis states and start cyclic exchange.</summary>
        bool Connect(const EtherCatConfig& config);
        /// <summary>Validate the whole config in one call ("Connect All Axis"): every configured
        /// slot is checked against the bus, and the bus is only brought up (IsConnected = true) if
        /// every configured slot is Ok — otherwise the service stays disconnected with the full
        /// per-slot breakdown returned.</summary>
        std::vector<AxisConnectResult> ConnectAllAxis(const EtherCatConfig& config);
        /// <summary>Same, but also surfaces the per-slot results for configured IoModules.</summary>
        std::vector<AxisConnectResult> ConnectAllAxis(const EtherCatConfig& config, std::vector<AxisConnectResult>& ioResults);
        /// <summary>Discovery-only bus scan. Doesn't touch Config/Axes/IsConnected.</summary>
        std::vector<DiscoveredSlaveInfo> ScanSlaves(const std::string& networkAdapter);
        /// <summary>Stop + JOIN the polling thread, then close the bus.</summary>
        void Disconnect();

        // ── CiA402 state machine ──
        /// <summary>Enable the axis: write "Enable Operation" (0x0F) in a single PDO write (the
        /// drive firmware walks the intermediate states itself — confirmed on real hardware), then
        /// confirm the drive actually reached Operation Enabled (up to 3 s — brake release can take
        /// 1-2 s) instead of assuming success.</summary>
        void ServoOn(int slavePosition);
        /// <summary>Disable the axis (drops back to "Ready to switch on").</summary>
        void ServoOff(int slavePosition);
        /// <summary>Clear a fault. Pulses the Fault Reset bit (rising edge required by CiA402).</summary>
        void FaultReset(int slavePosition);
        /// <summary>Switch the axis's mode of operation (do it while the axis is stopped).</summary>
        void SetMode(int slavePosition, OperationMode mode);

        // ── Motion — Profile Position (pp) mode ──
        /// <summary>Move to an absolute position (axis units; velocity units/s; accel/decel time s).
        /// The drive generates its own ramp — the master writes the target once.</summary>
        void MoveAbsolute(int slavePosition, double positionUnits, double profileVelocity, double accelTimeS, double decelTimeS);
        /// <summary>Move by a relative distance from the last polled actual position.</summary>
        void MoveRelative(int slavePosition, double deltaUnits, double profileVelocity, double accelTimeS, double decelTimeS);
        /// <summary>Jog toward the configured software limit (or ±1e6 units) in the given direction
        /// (+1/-1) as a pp move toward a far target — the default RxPdo of the Leadshine drives maps
        /// only Target Position, not Target Velocity, so a pv-mode jog would need PDO reassignment.
        /// Stop the jog with Stop().</summary>
        void Jog(int slavePosition, int direction, double speed, double accelTimeS = 0.2, double decelTimeS = 0.2);
        /// <summary>Move two axes so they trace a straight line (same accel/decel TIME on both,
        /// both triggered within the same bus-lock hold). Returns true if the move was skipped
        /// (negligible distance).</summary>
        bool MoveLinear(int slavePos1, double pos1, int slavePos2, double pos2, double velocity, double accelTimeS, double decelTimeS);
        /// <summary>Start homing using the axis's configured HomingMethod (object 0x6098).</summary>
        void StartHome(int slavePosition);

        // ── Digital I/O ──
        /// <summary>Raw module idents the coupler reported (0xF050), in slot order.</summary>
        std::vector<uint32_t> GetDetectedModuleIdents(int slavePosition);
        /// <summary>Same, resolved against an ESI module catalog; an ident not in the catalog still
        /// returns an entry with DisplayName "Unknown module 0x...".</summary>
        std::vector<EsiModuleInfo> GetDetectedModules(int slavePosition, const std::vector<EsiModuleInfo>& catalog);
        /// <summary>Flip one output channel of a connected I/O module (read-modify-write against the
        /// tracked Outputs mask).</summary>
        void SetDigitalOutput(int slavePosition, int channel, bool value);
        /// <summary>Flip one output channel of a single module slot of a modular coupler.</summary>
        void SetSlotOutput(int slavePosition, int slot, int channel, bool value);

        // ── Stop ──
        /// <summary>Halt the axis smoothly (Controlword bit 8), keeping Operation Enabled.</summary>
        void Stop(int slavePosition);
        /// <summary>Quick stop the axis (Controlword bit 2 = 0).</summary>
        void EmergencyStop(int slavePosition);
        void StopAllAxes();

        /// <summary>Wait until the axis reports Target Reached (Statusword bit 10). timeoutMs 0 = no timeout.</summary>
        bool WaitForMotionComplete(int slavePosition, int timeoutMs = 30000);

        /// <summary>Get axis / I/O module state by slave position (nullptr if none). An toàn khi
        /// gọi trong lúc kết nối lại.</summary>
        std::shared_ptr<AxisState> GetAxisState(int slavePosition) const;
        std::shared_ptr<IoModuleState> GetIoModuleState(int slavePosition) const;

        /// <summary>Cyclic-timer diagnostics. Safe to call anytime.</summary>
        CyclicStats GetCyclicStats();

        /// <summary>Stop polling and close the bus (also done by the destructor).</summary>
        void Dispose();

    private:
        /// <summary>See the C# history: on real Leadshine drives 0x606C/0x6077 are NOT in the
        /// default TxPdo, so refreshing them means SDO reads from the polling loop. Left disabled
        /// as the conservative default until verified on real hardware.</summary>
        static constexpr bool PollVelocityTorque = false;
        static constexpr int VelocityTorquePollDivider = 250;
        /// <summary>How far (axis units) a Jog without a configured software limit reaches for.</summary>
        static constexpr double JogReachUnits = 1'000'000;

        void WriteDriveParameters(const AxisConfig& axisConfig);
        bool WaitForOperationEnabled(int slavePosition, int timeoutMs);
        void WriteMoveCommand(const EtherCatConfig* config, int slavePosition, double positionUnits, double profileVelocity,
                              double accelTimeS, double decelTimeS);
        void ClearNewSetpoint(int slavePosition);
        std::vector<std::shared_ptr<IoSlotState>> BuildSlotStates(int slavePosition, const std::vector<EsiModuleInfo>& catalog);
        void StartCyclicExchange();
        void StopCyclicExchange();
        void PollAxisState(AxisState& axis, const EtherCatConfig* config, bool pollVelocityTorque);
        void PollIoModuleState(IoModuleState& io);
        void WriteControlword(int slavePosition, uint16_t controlword);
        void SetConfig(const EtherCatConfig& config);
        void Log(const std::string& message);

        std::shared_ptr<IEtherCatBus> m_bus;
        std::mutex m_busLock;                 // _busLock (SemaphoreSlim) của C#
        mutable std::mutex m_stateMutex;      // bảo vệ m_axes / m_ioModules / m_config
        std::vector<std::shared_ptr<AxisState>> m_axes;
        std::vector<std::shared_ptr<IoModuleState>> m_ioModules;
        std::shared_ptr<const EtherCatConfig> m_config;
        std::atomic<bool> m_isConnected{ false };
        std::mutex m_threadMutex;             // bảo vệ việc tạo/dừng m_cyclicThread
        std::jthread m_cyclicThread;
        bool m_disposed = false;
        int m_pollCounter = 0;
    };
}
