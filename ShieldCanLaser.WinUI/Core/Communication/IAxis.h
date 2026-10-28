#pragma once
#include <memory>
#include <stop_token>
#include <string>

#include "Communication/AxisConfig.h"

namespace scs
{
    /// <summary>
    /// Trừu tượng hoá một trục. Hai lớp hiện thực: <c>EtherCatAxis</c> (servo CiA402 trên bus
    /// SOEM — cả 3 trục máy thật) và <c>FakeAxis</c> (mô phỏng). Routine và interlock viết một lần,
    /// không phụ thuộc thư viện EtherCAT.
    ///
    /// <para>Đơn vị vị trí và tốc độ là <b>đơn vị của trục</b> — mm hoặc độ (xem
    /// <see cref="AxisConfig::Unit"/>). Tốc độ tính theo đơn vị/giây.</para>
    ///
    /// <para>Mọi thao tác dài đều nhận <c>std::stop_token</c>: SafetyMonitor huỷ token
    /// chung khi EMG/alarm, và đó là cơ chế dừng duy nhất mà routine cần biết. Các hàm dài là HÀM
    /// CHẶN — gọi từ luồng nền; các getter trạng thái đọc được từ mọi luồng (UI đọc theo chu kỳ).</para>
    /// </summary>
    class IAxis
    {
    public:
        virtual ~IAxis() = default;

        virtual const std::string& Name() const = 0;
        /// <summary>Cùng một đối tượng với phần tử trong <c>SystemData::Axes</c> — sửa ở trang Setting có
        /// hiệu lực ngay.</summary>
        virtual std::shared_ptr<AxisConfig> Config() const = 0;

        virtual bool IsConnected() const = 0;
        virtual bool ServoOn() const = 0;
        virtual bool Busy() const = 0;
        virtual bool Alarm() const = 0;
        virtual bool OrgOk() const = 0;

        /// <summary>Giới hạn cứng chiều dương (POT).</summary>
        virtual bool FLS() const = 0;
        /// <summary>Giới hạn cứng chiều âm (NOT).</summary>
        virtual bool RLS() const = 0;

        virtual double CurrentPos() const = 0;

        /// <summary>Mã lỗi thô của driver, hiển thị trong Detail của alarm.</summary>
        virtual int ErrorCode() const = 0;

        virtual bool Connect() = 0;
        virtual void Disconnect() = 0;

        virtual bool SetServoOn(bool on) = 0;

        /// <summary>Về gốc. Bên gọi chịu trách nhiệm interlock; chiều về gốc do tham số trong driver quyết định.</summary>
        virtual bool Home(std::stop_token ct) = 0;

        /// <summary>Di chuyển tuyệt đối rồi chờ tới đích.</summary>
        /// <param name="gentle">Lệnh chậm này giữ nguyên THỜI GIAN ramp ở Setting, tức gia tốc giảm theo tốc độ lệnh.
        /// Mặc định mọi lệnh dùng chung gia tốc của mức Auto cao (lệnh chậm tới tốc độ của nó sớm hơn).</param>
        virtual bool MoveAbs(double pos, double speed, std::stop_token ct, bool gentle = false) = 0;

        /// <summary>
        /// Di chuyển TƯƠNG ĐỐI (Inching) một khoảng <paramref name="delta"/> tính từ vị trí HIỆN
        /// TẠI rồi chờ tới đích — soft limit vẫn kiểm theo đích tuyệt đối quy ra từ đó, không phải
        /// theo độ lớn delta. Trục quay quấn vòng nhích ĐÚNG delta (kể cả &gt; nửa vòng), không đổi
        /// thành đường ngắn như Move Abs.
        /// </summary>
        virtual bool MoveInc(double delta, double speed, std::stop_token ct) = 0;

        /// <summary>Jog liên tục. dir: +1 / −1 / 0 = dừng.</summary>
        virtual void Jog(int dir, double speed) = 0;

        /// <summary>Dừng có giảm tốc.</summary>
        virtual void Stop() = 0;

        virtual bool ResetAlarm() = 0;

        /// <summary>Đọc lại trạng thái từ driver. Gọi từ vòng quét của MotionConnect.</summary>
        virtual void Refresh() = 0;
    };
}
