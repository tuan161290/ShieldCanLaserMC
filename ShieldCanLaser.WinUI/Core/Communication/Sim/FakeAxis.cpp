#include "Communication/Sim/FakeAxis.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>

#include "Communication/AxisAngle.h"
#include "Infor/AlarmManager.h"

namespace scs
{
    namespace
    {
        std::atomic<double>& SpeedFactorRef()
        {
            static std::atomic<double> value{ 1.0 };
            return value;
        }

        int Sign(double v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

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

    double FakeAxis::SpeedFactor() { return SpeedFactorRef().load(); }
    void FakeAxis::SetSpeedFactor(double v) { SpeedFactorRef() = v; }

    FakeAxis::FakeAxis(std::shared_ptr<AxisConfig> config)
        : m_name(config->Name), m_config(std::move(config)),
          m_rng(static_cast<unsigned>(std::hash<std::string>{}(m_name)))
    {
        double pos = m_config->StandbyPos;

        // Máy thật lúc bật nguồn không nằm sẵn ở gốc. Nếu vị trí chờ trùng gốc thì đặt lệch
        // ra để chuỗi về gốc có quãng đường thật sự phải chạy — không thì bước O2 hoàn tất
        // tức thời và chốt chặn Z_SAFE không được kiểm thử gì cả.
        if (std::fabs(pos - m_config->HomeOffset) < 1e-6)
            pos = Clamp(m_config->HomeOffset - m_config->HomeDir * 15.0);
        m_pos = pos;
    }

    double FakeAxis::Clamp(double v) const
    {
        return m_config->WrapDegrees ? v
             : v < m_config->SoftLimitNeg ? m_config->SoftLimitNeg
             : v > m_config->SoftLimitPos ? m_config->SoftLimitPos
             : v;
    }

    bool FakeAxis::FLS() const { return !m_config->WrapDegrees && m_pos.load() >= m_config->SoftLimitPos + HardLimitMargin; }
    bool FakeAxis::RLS() const { return !m_config->WrapDegrees && m_pos.load() <= m_config->SoftLimitNeg - HardLimitMargin; }

    double FakeAxis::CurrentPos() const
    {
        return m_config->WrapDegrees ? AxisAngle::Wrap(m_pos.load()) : m_pos.load();
    }

    bool FakeAxis::Connect()
    {
        m_connected = true;
        {
            std::lock_guard<std::mutex> lock(m_tickMutex);
            m_lastTickSec = m_clock.ElapsedSeconds();
        }
        return true;
    }

    void FakeAxis::Disconnect()
    {
        m_connected = false;
        m_servoOn = false;
        m_orgOk = false;
    }

    bool FakeAxis::SetServoOn(bool on)
    {
        if (!IsConnected()) return false;
        m_servoOn = on;   // tắt servo không làm mất gốc — khớp §4.1 (Tuấn bỏ quy tắc cũ 2026-09-26)
        return true;
    }

    bool FakeAxis::Home(std::stop_token ct)
    {
        if (!IsConnected() || !ServoOn() || Alarm()) return false;

        m_orgOk = false;

        // Cảm biến gốc nằm ở HomeOffset, trong hành trình cho phép — trục chạy TỚI nó chứ
        // không chạy một quãng cố định rồi tuyên bố đã về gốc. Mô phỏng kiểu "chạy 25 mm rồi
        // nhảy về gốc" từng làm trục vọt qua cả cữ hành trình cứng và sinh alarm giả.
        m_homeTarget = Clamp(m_config->HomeOffset);
        // Về gốc luôn chạy ở tốc độ CHẬM cố định — không theo nút chọn Nhanh/Chậm của thao
        // tác tay, an toàn không được phép phụ thuộc việc operator đang để chế độ nào.
        m_speed = m_config->JogSpeedSlow;
        m_mode = Mode::Home;
        m_busy = true;

        base::Stopwatch sw;
        {
            BusyReset busyReset{ m_busy };
            while (m_mode.load() == Mode::Home)
            {
                if (ct.stop_requested()) { Abort(); return false; }

                if (sw.ElapsedMilliseconds() > m_config->HomeTimeoutMs)
                {
                    Abort();
                    AlarmManager::RaiseAxisEvent(m_name, AxisFault::HomeTimeout,
                        std::format("Timeout {} ms", m_config->HomeTimeoutMs));
                    return false;
                }

                if (Alarm())
                {
                    Abort();
                    AlarmManager::RaiseAxis(m_name, AxisFault::DriverAlarm, std::format("0x{:04X}", ErrorCode()));
                    return false;
                }

                base::Delay(20);
            }
        }

        return OrgOk();
    }

    bool FakeAxis::MoveAbs(double pos, double speed, std::stop_token ct, bool gentle)
    {
        if (gentle) { ++m_gentleMoves; m_lastGentleSpeed = speed; }
        double cur = m_pos.load();
        double raw = m_config->WrapDegrees
            ? cur + AxisAngle::ShortestDelta(AxisAngle::Wrap(cur), pos)
            : pos;
        return RunAbs(raw, pos, speed, ct);
    }

    /// <summary><paramref name="rawTarget"/> là đích trong hệ chưa quấn; <paramref name="shownPos"/>
    /// là con số người gọi đã yêu cầu, chỉ dùng cho thông báo lỗi và phép kiểm tra cuối.</summary>
    bool FakeAxis::RunAbs(double rawTarget, double shownPos, double speed, std::stop_token ct)
    {
        double pos = shownPos;
        if (!IsConnected() || !ServoOn() || Alarm()) return false;

        if (!m_config->InSoftLimit(pos))
        {
            AlarmManager::RaiseAxisEvent(m_name, AxisFault::SoftLimit,
                std::format("Target {:.3f}{} outside [{}, {}]", pos, m_config->Unit,
                            Trim3(m_config->SoftLimitNeg), Trim3(m_config->SoftLimitPos)));
            return false;
        }
        if (!OrgOk())
        {
            AlarmManager::RaiseAxisEvent(m_name, AxisFault::NotHomed);
            return false;
        }

        m_target = rawTarget;
        m_speed = std::max(0.1, speed);
        m_mode = Mode::MoveAbs;
        m_busy = true;

        base::Stopwatch sw;
        {
            BusyReset busyReset{ m_busy };
            while (m_mode.load() == Mode::MoveAbs)
            {
                if (ct.stop_requested()) { Abort(); return false; }

                if (sw.ElapsedMilliseconds() > m_config->MoveTimeoutMs)
                {
                    Abort();
                    AlarmManager::RaiseAxisEvent(m_name, AxisFault::MoveTimeout,
                        std::format("{:.3f}{} short of target", std::fabs(m_target.load() - m_pos.load()), m_config->Unit));
                    return false;
                }

                if (Alarm())
                {
                    Abort();
                    AlarmManager::RaiseAxis(m_name, AxisFault::DriverAlarm, std::format("0x{:04X}", ErrorCode()));
                    return false;
                }

                base::Delay(10);
            }
        }

        return m_config->WrapDegrees
            ? AxisAngle::Distance(CurrentPos(), pos) < 0.01
            : std::fabs(m_pos.load() - pos) < 0.01;
    }

    bool FakeAxis::MoveInc(double delta, double speed, std::stop_token ct)
    {
        return RunAbs(m_pos.load() + delta, CurrentPos() + delta, speed, ct);
    }

    void FakeAxis::Jog(int dir, double speed)
    {
        if (!IsConnected() || !ServoOn() || Alarm()) return;

        if (dir == 0)
        {
            if (m_mode.load() == Mode::Jog) { m_mode = Mode::Idle; m_busy = false; }
            m_jogDir = 0;
            return;
        }

        m_jogDir = Sign(dir);
        m_speed = std::max(0.1, speed);
        m_mode = Mode::Jog;
        m_busy = true;
    }

    bool FakeAxis::ResetAlarm()
    {
        m_simDriverAlarm = false;
        AlarmManager::ClearConditionAxis(m_name, AxisFault::DriverAlarm);
        return true;
    }

    void FakeAxis::Abort()
    {
        m_mode = Mode::Idle;
        m_jogDir = 0;
        m_busy = false;
    }

    void FakeAxis::Refresh()
    {
        std::lock_guard<std::mutex> lock(m_tickMutex);

        double now = m_clock.ElapsedSeconds();
        double dt = now - m_lastTickSec;
        m_lastTickSec = now;

        if (dt <= 0 || dt > 1.0) dt = 0.05;      // bỏ qua bước nhảy khi debug dừng lâu
        if (!IsConnected() || !ServoOn()) return;

        double v = m_speed.load() * (m_simStall.load() ? 0.05 : SpeedFactor());
        double step = v * dt;
        double pos = m_pos.load();

        switch (m_mode.load())
        {
            case Mode::MoveAbs:
            {
                double remain = m_target.load() - pos;
                if (std::fabs(remain) <= step)
                {
                    // Sai số lặp lại ±5 µm — để logic phía trên không vô tình phụ thuộc
                    // vào việc trục dừng đúng tuyệt đối, thứ không bao giờ xảy ra thật.
                    double r = std::uniform_real_distribution<double>(0.0, 1.0)(m_rng);
                    m_pos = m_target.load() + (r - 0.5) * 0.01;
                    m_mode = Mode::Idle;
                }
                else
                {
                    m_pos = pos + Sign(remain) * step;
                }
                break;
            }

            case Mode::Jog:
            {
                double next = pos + m_jogDir.load() * step;
                // Trục quay jog được vô hạn theo cả hai chiều — không có cữ để dừng lại.
                if (!m_config->WrapDegrees)
                {
                    if (next > m_config->SoftLimitPos) { next = m_config->SoftLimitPos; Abort(); }
                    if (next < m_config->SoftLimitNeg) { next = m_config->SoftLimitNeg; Abort(); }
                }
                m_pos = next;
                break;
            }

            case Mode::Home:
            {
                double remain = m_homeTarget.load() - pos;
                if (std::fabs(remain) <= step)
                {
                    m_pos = m_config->HomeOffset;
                    m_orgOk = true;
                    m_mode = Mode::Idle;
                }
                else
                {
                    m_pos = pos + Sign(remain) * step;
                }
                break;
            }

            case Mode::Idle:
                break;
        }
    }
}
