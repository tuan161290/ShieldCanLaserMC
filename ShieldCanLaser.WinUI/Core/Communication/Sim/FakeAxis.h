#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <string>

#include "Base/Async.h"
#include "Communication/IAxis.h"

namespace scs
{
    /// <summary>
    /// Trục mô phỏng — cho phép viết và kiểm thử toàn bộ chuỗi Origin, chuỗi Auto, xử lý NG và
    /// Pause/Resume trước khi có phần cứng (PLAN.md §10, Simulation mode).
    ///
    /// <para>Mô phỏng theo thời gian thật: vị trí tiến dần về đích với tốc độ đã lệnh, cập nhật
    /// trong <see cref="Refresh"/> đúng như driver thật được vòng quét đọc trạng thái. Nhờ vậy
    /// timeout, interlock và điều kiện chờ chạy trên FakeAxis có ý nghĩa thật, chứ không phải
    /// hoàn tất tức thời làm mọi thứ luôn "đúng". Hệ quả: Home/MoveAbs/MoveInc chỉ tới đích khi CÓ
    /// MỘT LUỒNG KHÁC đang gọi Refresh() theo chu kỳ (vòng quét của MotionConnect).</para>
    ///
    /// <para>Các cờ <c>Sim*</c> để bơm lỗi từ trang Manual: chúng là cách duy nhất kiểm thử được
    /// nhánh xử lý alarm mà không phải rút cáp máy thật.</para>
    /// </summary>
    class FakeAxis : public IAxis
    {
    public:
        explicit FakeAxis(std::shared_ptr<AxisConfig> config);
        FakeAxis(const FakeAxis&) = delete;
        FakeAxis& operator=(const FakeAxis&) = delete;

        const std::string& Name() const override { return m_name; }
        std::shared_ptr<AxisConfig> Config() const override { return m_config; }

        // ── Cờ bơm lỗi cho mô phỏng ──
        bool SimDisconnected() const { return m_simDisconnected.load(); }
        void SetSimDisconnected(bool v) { m_simDisconnected = v; }

        /// <summary>Bật/tắt qua nút ALM ở trang Manual — <see cref="Alarm"/> chỉ là phép chiếu qua cờ này.</summary>
        bool SimDriverAlarm() const { return m_simDriverAlarm.load(); }
        void SetSimDriverAlarm(bool v) { m_simDriverAlarm = v; }

        /// <summary>Chạy chậm 20 lần để kích hoạt nhánh timeout — không nhân <see cref="SpeedFactor"/>.</summary>
        bool SimStall() const { return m_simStall.load(); }
        void SetSimStall(bool v) { m_simStall = v; }

        /// <summary>
        /// Nhân tốc độ mọi trục giả. App chạy thường để 1 (đúng tốc độ thật); self-test đặt rất lớn để trục tới đích trong
        /// một vòng quét (Tuấn 2026-09-29 — thời gian self-test gần như toàn là chờ trục giả chạy).
        /// </summary>
        static double SpeedFactor();
        static void SetSpeedFactor(double v);

        /// <summary>Số lệnh chạy dịu (ramp theo tốc độ lệnh) và tốc độ của lệnh gần nhất — cho self-test.</summary>
        int GentleMoves() const { return m_gentleMoves.load(); }
        double LastGentleSpeed() const { return m_lastGentleSpeed.load(); }

        bool IsConnected() const override { return m_connected.load() && !m_simDisconnected.load(); }
        bool ServoOn() const override { return m_servoOn.load(); }
        bool Busy() const override { return m_busy.load(); }
        bool Alarm() const override { return m_simDriverAlarm.load(); }
        bool OrgOk() const override { return m_orgOk.load(); }
        bool FLS() const override;
        bool RLS() const override;

        /// <summary>Toạ độ máy: trục quay thì quấn về [0, 360), trục thẳng thì đúng vị trí thô.</summary>
        double CurrentPos() const override;

        int ErrorCode() const override { return m_simDriverAlarm.load() ? 0x1601 : 0; }

        bool Connect() override;
        void Disconnect() override;

        bool SetServoOn(bool on) override;
        bool Home(std::stop_token ct) override;

        /// <summary>
        /// Chạy tuyệt đối. Với trục quay, đích được quy về vòng gần vị trí hiện tại nhất — mô
        /// phỏng phải đi đúng đường ngắn như <c>EtherCatAxis</c>, nếu không thì phép đo thời gian
        /// chu kỳ và mọi kiểm thử về góc trong <c>--selftest</c> đều chạy trên hành vi không có
        /// thật.
        /// </summary>
        bool MoveAbs(double pos, double speed, std::stop_token ct, bool gentle = false) override;

        /// <summary>
        /// Di chuyển TƯƠNG ĐỐI (Inching) — cộng dồn rồi chạy tới đó.
        ///
        /// <para>Đi thẳng vào RunAbs chứ không qua <see cref="MoveAbs"/>:
        /// trục quay phải nhích đúng <paramref name="delta"/> đã yêu cầu, kể cả khi nó lớn hơn nửa
        /// vòng. Qua Move Abs thì phép "đi đường ngắn" sẽ đổi một lệnh nhích +200° thành −160° —
        /// đúng tư thế cuối nhưng sai hẳn thao tác người vận hành vừa bấm.</para>
        /// </summary>
        bool MoveInc(double delta, double speed, std::stop_token ct) override;

        void Jog(int dir, double speed) override;
        void Stop() override { Abort(); }
        bool ResetAlarm() override;

        /// <summary>Bước mô phỏng theo thời gian thực. MotionConnect gọi mỗi vòng quét.</summary>
        void Refresh() override;

    private:
        enum class Mode { Idle, MoveAbs, Jog, Home };

        /// <summary>
        /// Cữ hành trình cứng đặt cách giới hạn mềm một khoảng, đúng như bố trí thật: soft limit
        /// là hàng rào của phần mềm, cảm biến cứng nằm sau nó vài mm làm lớp cuối cùng.
        /// </summary>
        static constexpr double HardLimitMargin = 5.0;

        /// <summary>Trục quay tự do không có hàng rào hành trình để mà kẹp vào.</summary>
        double Clamp(double v) const;
        bool RunAbs(double rawTarget, double shownPos, double speed, std::stop_token ct);
        void Abort();

        std::string m_name;
        std::shared_ptr<AxisConfig> m_config;

        base::Stopwatch m_clock;
        std::mutex m_tickMutex;          // Refresh() gọi được từ nhiều luồng: m_rng + m_lastTickSec
        std::mt19937 m_rng;
        double m_lastTickSec{ 0 };

        std::atomic<Mode> m_mode{ Mode::Idle };
        std::atomic<double> m_target{ 0 };
        std::atomic<double> m_speed{ 0 };
        std::atomic<int> m_jogDir{ 0 };
        std::atomic<double> m_homeTarget{ 0 };

        /// <summary>
        /// Vị trí THÔ, chưa quấn — tương ứng bộ đếm của driver thật trong <c>EtherCatAxis</c>.
        /// Mô phỏng phải giữ nó chưa quấn thì phép "đi đường ngắn" mới có gì để đo.
        /// </summary>
        std::atomic<double> m_pos{ 0 };

        std::atomic<bool> m_simDisconnected{ false };
        std::atomic<bool> m_simDriverAlarm{ false };
        std::atomic<bool> m_simStall{ false };
        std::atomic<bool> m_connected{ false };
        std::atomic<bool> m_servoOn{ false };
        std::atomic<bool> m_busy{ false };
        std::atomic<bool> m_orgOk{ false };
        std::atomic<int> m_gentleMoves{ 0 };
        std::atomic<double> m_lastGentleSpeed{ 0 };
    };
}
