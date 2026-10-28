#include "Communication/EtherCat/EtherCatAxis.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

#include "Base/Async.h"
#include "Base/Text.h"
#include "Communication/AxisAngle.h"
#include "Infor/AlarmManager.h"
#include "Utility/Log/LogWorker.h"

namespace scs
{
    namespace
    {
        /// <summary>Định dạng "0.###" của C#: tối đa 3 chữ số thập phân, bỏ số 0 thừa.</summary>
        std::string Trim3(double v)
        {
            std::string s = std::format("{:.3f}", v);
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
            return s;
        }

        /// <summary>finally { Busy = false; }</summary>
        struct BusyReset
        {
            std::atomic<bool>& flag;
            ~BusyReset() { flag = false; }
        };
    }

    EtherCatAxis::EtherCatAxis(std::shared_ptr<AxisConfig> config, std::shared_ptr<EtherCatBusManager> bus)
        : m_name(config->Name), m_config(std::move(config)), m_bus(std::move(bus))
    {
    }

    int EtherCatAxis::Address() const
    {
        return ecm::AxisAddress::Make(m_config->SlavePosition, m_config->AxisChannel);
    }

    std::shared_ptr<ecm::AxisState> EtherCatAxis::State() const
    {
        auto service = m_bus->Service();
        return service ? service->GetAxisState(Address()) : nullptr;
    }

    double EtherCatAxis::RawPos() const
    {
        auto st = State();
        return st ? st->ActualPositionUnits.load() : 0.0;
    }

    template <class F>
    void EtherCatAxis::FireAndForget(F&& command) const
    {
        try { command(); }
        catch (const std::exception& ex) { LogCommandError(ex.what()); }
        catch (...) { LogCommandError("unknown error"); }
    }

    void EtherCatAxis::LogCommandError(const char* what) const
    {
        LogWorker::Instance().RecordLog("[" + m_name + "] EtherCAT command error: " + what, LogLevel::Error, "EtherCAT");
    }

    void EtherCatAxis::Refresh()
    {
        auto st = State();
        bool connected = m_bus->IsConnected() && st != nullptr && st->IsConnected.load();

        // Mất kết nối là mất gốc: không biết driver có bị khởi động lại trong lúc đó không. Bus mở lại (kể cả
        // đóng-mở liền trong một lệnh Connect All Axis) cũng vậy — so lượt mở bus lúc về gốc.
        if (!connected && m_isConnected.load()) m_orgOk = false;
        if (m_orgOk.load() && m_homedEpoch.load() != m_bus->ConnectionEpoch()) m_orgOk = false;
        m_isConnected = connected;
        if (st == nullptr) { m_servoOn = false; return; }

        m_servoOn = st->OperationEnabled.load();
        m_alarm = st->Fault.load();
        m_errorCode = st->ErrorCode.load();

        double raw = st->ActualPositionUnits.load();
        m_pos = m_config->WrapDegrees ? AxisAngle::Wrap(raw) : raw;

        m_fls = !m_config->WrapDegrees && raw >= m_config->SoftLimitPos + HardLimitMargin;
        m_rls = !m_config->WrapDegrees && raw <= m_config->SoftLimitNeg - HardLimitMargin;
    }

    bool EtherCatAxis::Connect()
    {
        bool busOk = m_bus->EnsureConnected();
        bool resultOk = false;
        for (const auto& r : m_bus->LastAxisResults())
        {
            if (r.Address() == Address()) { resultOk = r.IsOk(); break; }
        }
        bool ok = busOk && resultOk;
        Refresh();
        // Không tự raise alarm — MotionConnect::ConnectAll() đã raise AxisFault::Disconnected cho trục lỗi.
        return ok;
    }

    void EtherCatAxis::Disconnect()
    {
        m_orgOk = false;
        m_servoOn = false;
        m_bus->Disconnect();
        m_isConnected = false;
    }

    bool EtherCatAxis::SetServoOn(bool on)
    {
        auto service = m_bus->Service();
        if (!IsConnected() || service == nullptr) return false;

        if (on)
        {
            // ServoOn của thư viện tự chờ Operation Enabled tới 3 s (có driver mất 1–2 s vì nhả phanh).
            service->ServoOn(Address());
            Refresh();
            if (!ServoOn())
                AlarmManager::RaiseAxisEvent(m_name, AxisFault::ServoOffUnexpected, "Cannot reach Operation Enabled — check STO/24V");
            return ServoOn();
        }

        service->ServoOff(Address());
        Refresh();
        return true;
    }

    bool EtherCatAxis::Home(std::stop_token ct)
    {
        auto service = m_bus->Service();
        if (!IsConnected() || !ServoOn() || Alarm() || service == nullptr) return false;

        const int slave = Address();
        m_orgOk = false;
        m_busy = true;
        BusyReset busyReset{ m_busy };
        base::Stopwatch sw;

        service->StartHome(slave);

        while (true)
        {
            if (ct.stop_requested())
            {
                FireAndForget([&] { service->Stop(slave); });
                return false;
            }

            auto st = State();
            Refresh();

            if (st == nullptr || !IsConnected())
                return false;

            if (st->Fault.load())
            {
                FireAndForget([&] { service->Stop(slave); });
                AlarmManager::RaiseAxisEvent(m_name, AxisFault::HomeTimeout,
                    std::format("Driver fault during homing, 0x{:04X}", ErrorCode()));
                return false;
            }

            if (sw.ElapsedMilliseconds() > m_config->HomeTimeoutMs)
            {
                FireAndForget([&] { service->Stop(slave); });
                AlarmManager::RaiseAxisEvent(m_name, AxisFault::HomeTimeout, std::format("Timeout {} ms", m_config->HomeTimeoutMs));
                return false;
            }

            // CiA402, mode Homing: statusword bit12 = Homing Attained, bit13 = Homing Error.
            if (st->FollowingErrorOrHomingError.load())
            {
                FireAndForget([&] { service->Stop(slave); });
                AlarmManager::RaiseAxisEvent(m_name, AxisFault::HomeTimeout, "Homing Error flag in statusword");
                return false;
            }

            if (st->SetpointAckOrHomingAttained.load())
            {
                // StartHome để lại bit4 (Homing start) trên controlword. Ở Profile Position, bit4 chính là
                // "New set-point" và driver chỉ chốt lệnh ở SƯỜN LÊN của nó — không hạ xuống thì lệnh chạy
                // đầu tiên sau về gốc có thể bị bỏ qua. ServoOn ghi lại đúng 0x0F (Enable Operation), hạ bit4.
                service->ServoOn(slave);
                m_homedEpoch = m_bus->ConnectionEpoch();
                m_orgOk = true;
                Refresh();
                return true;
            }

            base::Delay(20);
        }
    }

    bool EtherCatAxis::MoveAbs(double pos, double speed, std::stop_token ct, bool gentle)
    {
        // Trục quay: chọn vòng gần vị trí THÔ hiện tại nhất — đi đường ngắn, 350° tới 10° là +20°.
        double rawPos = RawPos();
        double raw = m_config->WrapDegrees ? rawPos + AxisAngle::ShortestDelta(AxisAngle::Wrap(rawPos), pos) : pos;
        return RunAbs(raw, pos, speed, gentle, ct);
    }

    bool EtherCatAxis::MoveInc(double delta, double speed, std::stop_token ct)
    {
        return RunAbs(RawPos() + delta, CurrentPos() + delta, speed, false, ct);
    }

    bool EtherCatAxis::RunAbs(double rawTarget, double shownPos, double speed, bool gentle, std::stop_token ct)
    {
        auto service = m_bus->Service();
        if (!IsConnected() || !ServoOn() || Alarm() || service == nullptr) return false;

        if (!m_config->InSoftLimit(shownPos))
        {
            AlarmManager::RaiseAxisEvent(m_name, AxisFault::SoftLimit,
                std::format("Target {:.3f}{} outside [{}, {}]", shownPos, m_config->Unit,
                            Trim3(m_config->SoftLimitNeg), Trim3(m_config->SoftLimitPos)));
            return false;
        }
        if (!OrgOk())
        {
            AlarmManager::RaiseAxisEvent(m_name, AxisFault::NotHomed);
            return false;
        }

        const int slave = Address();
        speed = std::max(0.001, std::fabs(speed));
        m_busy = true;
        BusyReset busyReset{ m_busy };
        base::Stopwatch sw;

        service->MoveAbsolute(slave, rawTarget, speed,
            RampTime(m_config->AccelTimeS, speed, gentle), RampTime(m_config->DecelTimeS, speed, gentle));

        double tol = std::max(1e-6, m_config->InPositionTolerance);
        while (true)
        {
            if (ct.stop_requested())
            {
                FireAndForget([&] { service->Stop(slave); });
                return false;
            }

            auto st = State();
            Refresh();
            if (st == nullptr || !IsConnected()) return false;

            if (st->Fault.load())
            {
                AlarmManager::RaiseAxis(m_name, AxisFault::DriverAlarm, std::format("0x{:04X}", ErrorCode()));
                return false;
            }

            // Không tin riêng bit Target Reached: ngay sau khi gửi lệnh, bit đó có thể còn sót từ lệnh
            // trước. Phải đồng thời đã ở trong dung sai quanh đích mới coi là tới nơi.
            double actual = st->ActualPositionUnits.load();
            if (st->TargetReached.load() && std::fabs(actual - rawTarget) <= tol)
                return true;

            if (sw.ElapsedMilliseconds() > m_config->MoveTimeoutMs)
            {
                FireAndForget([&] { service->Stop(slave); });
                AlarmManager::RaiseAxisEvent(m_name, AxisFault::MoveTimeout,
                    std::format("{:.3f}{} short of target", std::fabs(rawTarget - actual), m_config->Unit));
                return false;
            }

            base::Delay(5);
        }
    }

    double EtherCatAxis::RampTime(double timeS, double speed, bool gentle) const
    {
        double t = std::max(0.01, timeS);
        if (gentle || m_config->MoveSpeed <= 1e-9) return t;
        return std::max(0.01, t * std::min(1.0, speed / m_config->MoveSpeed));
    }

    void EtherCatAxis::Jog(int dir, double speed)
    {
        auto service = m_bus->Service();
        if (service == nullptr) return;
        const int slave = Address();

        if (dir == 0)
        {
            FireAndForget([&] { service->Stop(slave); });
            m_busy = false;
            return;
        }

        if (!IsConnected() || !ServoOn() || Alarm()) return;

        // Jog = lệnh Profile Position tới một đích xa, dừng bằng Halt (cách của thư viện EtherCAT — PDO mặc
        // định của driver Leadshine không có Target Velocity).
        //  • Trục thẳng ĐÃ về gốc: đích là giới hạn mềm, jog tự dừng ở hàng rào; đã ở ngoài hàng rào phía
        //    đó thì không chạy (đích nằm phía sau lưng — lệnh sẽ kéo trục đi NGƯỢC chiều nút bấm).
        //  • Chưa về gốc hoặc trục quay quấn vòng: toạ độ chưa có nghĩa/không có hàng rào, đi xa theo đúng
        //    chiều jog; chặn quãng để đích không tràn int32 của 0x607A.
        int sign = dir > 0 ? 1 : -1;
        double target;
        if (!m_config->WrapDegrees && OrgOk())
        {
            target = sign > 0 ? m_config->SoftLimitPos : m_config->SoftLimitNeg;
            if ((target - RawPos()) * sign <= 0) return;
        }
        else
        {
            double cpu = m_config->CountsPerUnit > 1e-9 ? m_config->CountsPerUnit : 1.0;
            target = RawPos() + sign * std::min(1e6, JogReachCounts / cpu);
        }

        speed = std::max(0.001, std::fabs(speed));
        m_busy = true;
        FireAndForget([&] {
            service->MoveAbsolute(slave, target, speed,
                std::max(0.01, m_config->AccelTimeS), std::max(0.01, m_config->DecelTimeS));
        });
    }

    void EtherCatAxis::Stop()
    {
        auto service = m_bus->Service();
        if (service == nullptr) return;
        FireAndForget([&] { service->Stop(Address()); });
        m_busy = false;
    }

    bool EtherCatAxis::ResetAlarm()
    {
        auto service = m_bus->Service();
        if (service == nullptr) return false;
        service->FaultReset(Address());

        // Driver cần vài chu kỳ để rời trạng thái Fault sau sườn lên của bit Fault Reset.
        base::Stopwatch sw;
        while (sw.ElapsedMilliseconds() < 500)
        {
            Refresh();
            if (!Alarm()) break;
            base::Delay(20);
        }

        if (!Alarm()) AlarmManager::ClearConditionAxis(m_name, AxisFault::DriverAlarm);
        return !Alarm();
    }

    std::string EtherCatAxis::DiagnosticText() const
    {
        auto st = State();
        if (st == nullptr) return std::format("{:<3} slave {}  — not on bus", m_name, ecm::AxisAddress::Text(Address()));

        std::vector<std::string> bits;
        if (st->ReadyToSwitchOn.load()) bits.push_back("RdySwOn");
        if (st->SwitchedOn.load()) bits.push_back("SwOn");
        if (st->OperationEnabled.load()) bits.push_back("OpEn");
        if (st->Fault.load()) bits.push_back("FAULT");
        if (st->VoltageEnabled.load()) bits.push_back("Volt");
        if (st->QuickStopActive.load()) bits.push_back("QStop");
        if (st->SwitchOnDisabled.load()) bits.push_back("SwOnDis");
        if (st->Warning.load()) bits.push_back("Warn");
        if (st->TargetReached.load()) bits.push_back("TgtReached");
        if (st->InternalLimitActive.load()) bits.push_back("IntLimit");
        if (st->SetpointAckOrHomingAttained.load()) bits.push_back("b12");
        if (st->FollowingErrorOrHomingError.load()) bits.push_back("b13");

        return std::format("{:<3} S{}  SW 0x{:04X}  mode {:>2}  pos {:>10}  err 0x{:04X}  {}",
            m_name, ecm::AxisAddress::Text(Address()), st->Statusword.load(), static_cast<int>(st->ModeOfOperationDisplay.load()),
            st->ActualPositionRaw.load(), st->ErrorCode.load(), base::Join(bits, " "));
    }
}
