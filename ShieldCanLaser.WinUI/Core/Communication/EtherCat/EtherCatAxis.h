#pragma once
#include <atomic>
#include <memory>
#include <string>

#include "Communication/EtherCat/EtherCatBusManager.h"
#include "Communication/IAxis.h"
#include "EtherCat/Models/AxisState.h"

namespace scs
{
    /// <summary>
    /// Trục servo EtherCAT (CiA402, Profile Position) — bọc <c>EtherCatBusManager::Service()</c>.
    /// Cả 14 trục dùng chung lớp này, khác nhau ở <c>AxisConfig::SlavePosition</c>. Khung lấy
    /// từ Partron CAM TEST, hợp đồng giữ đúng <see cref="IAxis"/> của ShieldCanAssy.
    ///
    /// <para><c>ecm::EtherCatMasterService</c> tự chạy luồng nền đọc PDO liên tục, nên
    /// <see cref="Refresh"/> ở đây chỉ chụp <c>ecm::AxisState</c> mới nhất vào các cờ trạng thái —
    /// không phải nơi đọc driver như ở trục Modbus cũ.</para>
    ///
    /// <para>Mọi lệnh dài trả <c>false</c> khi bị huỷ, KHÔNG ném <c>base::OperationCanceled</c>
    /// — routine dựa vào giá trị trả về, giống <c>FakeAxis</c>.</para>
    /// </summary>
    class EtherCatAxis : public IAxis
    {
    public:
        EtherCatAxis(std::shared_ptr<AxisConfig> config, std::shared_ptr<EtherCatBusManager> bus);
        EtherCatAxis(const EtherCatAxis&) = delete;
        EtherCatAxis& operator=(const EtherCatAxis&) = delete;

        const std::string& Name() const override { return m_name; }
        std::shared_ptr<AxisConfig> Config() const override { return m_config; }

        bool IsConnected() const override { return m_isConnected.load(); }
        bool ServoOn() const override { return m_servoOn.load(); }
        bool Busy() const override { return m_busy.load(); }
        bool Alarm() const override { return m_alarm.load(); }
        bool OrgOk() const override { return m_orgOk.load(); }
        bool FLS() const override { return m_fls.load(); }
        bool RLS() const override { return m_rls.load(); }
        /// <summary>Toạ độ máy: trục quay quấn vòng thì về [0, 360), còn lại đúng vị trí driver báo.</summary>
        double CurrentPos() const override { return m_pos.load(); }
        int ErrorCode() const override { return m_errorCode.load(); }

        void Refresh() override;
        bool Connect() override;
        void Disconnect() override;

        bool SetServoOn(bool on) override;
        bool Home(std::stop_token ct) override;
        bool MoveAbs(double pos, double speed, std::stop_token ct, bool gentle = false) override;

        /// <summary>
        /// Inching — nhích đúng <paramref name="delta"/> tính từ vị trí hiện tại. Đi thẳng vào
        /// RunAbs chứ không qua <see cref="MoveAbs"/>: với trục quay, Move Abs
        /// sẽ đổi một lệnh nhích +200° thành −160° — đúng tư thế cuối nhưng sai thao tác vừa bấm.
        /// </summary>
        bool MoveInc(double delta, double speed, std::stop_token ct) override;

        void Jog(int dir, double speed) override;
        void Stop() override;
        bool ResetAlarm() override;

        /// <summary>
        /// Một dòng chẩn đoán CiA402 cho trang Manual: slave, statusword thô + tên các bit đang bật, mode
        /// đang chạy (0x6061), vị trí counts, mã lỗi (0x603F). Dùng lúc ra máy để đối chiếu với tài liệu driver.
        /// </summary>
        std::string DiagnosticText() const;

    private:
        /// <summary>Thư viện chưa đưa Digital Inputs (0x60FD — POT/NOT/HOME) lên AxisState, nên giới hạn
        /// cứng suy ra từ vị trí: vượt giới hạn mềm quá chừng này coi như đã tới cữ. Cùng cách
        /// Partron CAM TEST và <c>FakeAxis</c>.</summary>
        static constexpr double HardLimitMargin = 5.0;

        /// <summary>Trần quãng "đi xa" của lệnh Jog trên trục quay quấn vòng, tính bằng counts — giữ đích
        /// trong int32 của 0x607A dù CountsPerUnit lớn.</summary>
        static constexpr double JogReachCounts = 1e9;

        std::shared_ptr<ecm::AxisState> State() const;
        /// <summary>Địa chỉ trục trên bus (slave + kênh — ecm::AxisAddress), tham số của mọi lệnh gửi service.</summary>
        int Address() const;

        /// <summary>Vị trí CHƯA quấn (counts / CountsPerUnit) — trục quay phải giữ nó để biết driver
        /// đang ở vòng nào khi chọn đường ngắn.</summary>
        double RawPos() const;

        /// <param name="rawTarget">Đích trong hệ chưa quấn — thứ gửi xuống driver.</param>
        /// <param name="shownPos">Con số người gọi yêu cầu — chỉ dùng cho soft limit và thông báo.</param>
        bool RunAbs(double rawTarget, double shownPos, double speed, bool gentle, std::stop_token ct);

        /// <summary>
        /// Thời gian ramp gửi xuống driver. Mặc định mọi lệnh dùng chung GIA TỐC của mức Auto cao
        /// (<c>AxisConfig::MoveSpeed</c> trong <c>AxisConfig::AccelTimeS</c>) — lệnh chậm hơn tới
        /// tốc độ của nó sớm hơn theo tỉ lệ, đúng quy ước của ShieldCanAssy. <paramref name="gentle"/> giữ nguyên
        /// THỜI GIAN ramp, tức gia tốc giảm theo tốc độ lệnh.
        /// </summary>
        double RampTime(double timeS, double speed, bool gentle) const;

        /// <summary>Lệnh "bắn-rồi-quên" của bản C# (Stop/Jog không chờ kết quả): ở đây service là hàm chặn
        /// ngắn nên chạy NGAY trên luồng gọi — giữ đúng thứ tự lệnh xuống driver — và lỗi được ghi log
        /// thay vì ném ra, không bao giờ bị nuốt im lặng.</summary>
        template <class F> void FireAndForget(F&& command) const;
        void LogCommandError(const char* what) const;

        std::string m_name;
        std::shared_ptr<AxisConfig> m_config;
        std::shared_ptr<EtherCatBusManager> m_bus;

        std::atomic<bool> m_isConnected{ false };
        std::atomic<bool> m_servoOn{ false };
        std::atomic<bool> m_busy{ false };
        std::atomic<bool> m_alarm{ false };
        std::atomic<bool> m_orgOk{ false };
        /// <summary>Lượt mở bus lúc về gốc xong — xem <c>EtherCatBusManager::ConnectionEpoch</c>.</summary>
        std::atomic<int> m_homedEpoch{ -1 };
        std::atomic<bool> m_fls{ false };
        std::atomic<bool> m_rls{ false };
        std::atomic<double> m_pos{ 0.0 };
        std::atomic<int> m_errorCode{ 0 };
    };
}
