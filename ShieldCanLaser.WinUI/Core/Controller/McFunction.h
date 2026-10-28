#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

#include "Base/Async.h"
#include "Communication/IIoDevice.h"
#include "Communication/MotionConnect.h"
#include "Controller/McStatus.h"
#include "Controller/SafetyMonitor.h"
#include "Data/Model/ModelData.h"
#include "Data/SystemData.h"

namespace scs
{
    /// <summary>
    /// Lớp phát lệnh chuyển động cấp cao. <b>Toàn bộ interlock nằm ở đây</b>, không nằm trong
    /// routine — routine chỉ mô tả trình tự công việc, còn "được phép chạy hay không" là câu hỏi chỉ
    /// có một chỗ trả lời. Thêm một điều kiện chống va mới thì sửa ở đây là mọi routine đều được bảo vệ.
    ///
    /// <para>Các hàm Move*/EnsureZSafe/Wait* là HÀM CHẶN — gọi từ luồng nền, không gọi từ luồng UI.</para>
    /// </summary>
    class McFunction
    {
    public:
        McFunction(MotionConnect& motion, SafetyMonitor& safety, McStatus& status, std::function<std::shared_ptr<SystemData>()> sys);
        McFunction(const McFunction&) = delete;
        McFunction& operator=(const McFunction&) = delete;

        std::shared_ptr<IIoDevice> Io() const { return m_io; }
        /// <summary>Đặt lúc khởi tạo, trước khi chia sẻ đối tượng.</summary>
        void SetIo(std::shared_ptr<IIoDevice> io) { m_io = std::move(io); }

        /// <summary>
        /// Kiểm tra được phép chạy trục này chưa. Trả về false kèm lý do đọc được để UI hiện thẳng cho
        /// operator thay vì im lặng không phản ứng.
        /// </summary>
        bool CanMove(std::string_view axisName, std::string& reason) const;

        /// <summary>
        /// Cùng chốt chặn với <see cref="CanMove"/>, cộng điều kiện cho nút "Về gốc"/"Move Abs" bấm RIÊNG
        /// LẺ ở trang Manual: Z phải ĐÃ VỀ GỐC THẬT, không chỉ đang đọc vị trí "trong vùng an toàn" — vị trí
        /// của một trục chưa về gốc là số rác từ lúc bật nguồn, <see cref="MotionConnect::ZSafeOk"/> theo nó
        /// có thể ĐÚNG NGẪU NHIÊN. Jog và Inching vẫn dùng thẳng <see cref="CanMove"/>: operator giữ tay và
        /// tự dừng được ngay.
        /// </summary>
        bool CanMoveAutonomous(std::string_view axisName, std::string& reason) const;

        bool Move(const std::string& axisName, double pos, double speed, std::stop_token ct);

        /// <summary>Như <see cref="Move"/> nhưng gia tốc giảm theo tốc độ lệnh — xem <c>IAxis::MoveAbs(gentle)</c>.</summary>
        bool MoveGentle(const std::string& axisName, double pos, double speed, std::stop_token ct);

        // ── Bit RUN ──
        //
        // STOP tắt bit: mọi trục nhận lệnh dừng ngay, chu trình KHÔNG thoát mà đứng chờ ngay trong lệnh chạy
        // đang dở. START bật lại bit thì lệnh dở đó được phát lại tới đúng đích cũ — chu trình không bao giờ
        // phải làm lại một bước từ giữa chừng.

        bool RunBit() const { return m_runBit.load(); }
        void SetRunBit(bool on);

        /// <summary>Gọi ở đầu Run của chu trình — đánh dấu mọi lệnh chạy trục phát ra từ luồng đó. Luồng đang
        /// chạy là chu trình Auto — chỉ lệnh của chu trình được chờ bit RUN. Lệnh bấm tay lúc tạm dừng bị từ
        /// chối, không xếp hàng rồi chạy cùng lúc với chu trình khi bấm START.
        /// <para>Cờ gắn theo LUỒNG (thay AsyncLocal): việc chạy song song bằng <c>base::WhenAll</c> từ luồng chu
        /// trình phải bọc mỗi việc trong <see cref="CycleFlowScope"/> để cờ đi theo — các hàm của lớp này đã tự làm.</para></summary>
        static void MarkCycleFlow();
        static bool InCycleFlow();

        /// <summary>Đặt cờ chu trình cho luồng hiện tại trong một khối, trả lại giá trị cũ khi ra khỏi khối.</summary>
        struct CycleFlowScope
        {
            explicit CycleFlowScope(bool inCycle);
            ~CycleFlowScope();
            CycleFlowScope(const CycleFlowScope&) = delete;
            CycleFlowScope& operator=(const CycleFlowScope&) = delete;
        private:
            bool m_before;
        };

        /// <summary>Chờ tới khi bit RUN bật. Bị huỷ thì ném <c>base::OperationCanceled</c>.</summary>
        void WaitRunBit(std::stop_token ct);

        /// <summary>Còn phải chờ bao nhiêu ms nữa cho đủ <paramref name="settleMs"/> kể từ lúc lệnh chạy cuối
        /// kết thúc. Hàm thuần để kiểm tra được luật mà không phải đo thời gian thật.
        /// <c>lastMoveEndedMs == NeverMoved</c> nghĩa là chưa có lệnh chạy nào.</summary>
        static int SettleRemainingMs(long long lastMoveEndedMs, long long nowMs, int settleMs);
        static const long long NeverMoved;

        /// <summary>Chờ cơ khí thôi rung sau lệnh chạy cuối cùng, trước khi ĐỌC ENCODER và kích Flow có camera
        /// — xem <see cref="SystemData::VisionSettleMs"/>. Bị huỷ thì ném <c>base::OperationCanceled</c>.</summary>
        void WaitSettle(std::stop_token ct);

        /// <summary>
        /// Điểm giảm tốc có được dùng hay không. Z dương là chiều ĐI XUỐNG, nên chặng giảm tốc chỉ có nghĩa
        /// khi điểm giảm tốc nằm HẲN giữa chỗ đang đứng và đích.
        /// </summary>
        static bool UseTwoStageZ(double now, double slowAt, double target) { return slowAt > 0 && now < slowAt && slowAt < target; }

        /// <summary>Chặng chậm khi ĐI LÊN — cùng phép xét của chiều xuống với "chỗ đang đứng" và "đích" đổi chỗ.</summary>
        static bool UseTwoStageZUp(double now, double slowAt, double target) { return UseTwoStageZ(target, slowAt, now); }

        /// <summary>Hạ trục Z qua HAI chặng: mức Auto CAO tới <paramref name="slowAt"/>, rồi mức THẤP nốt đoạn còn
        /// lại. Không dùng được điểm giảm tốc thì hạ một mạch ở mức cao.</summary>
        bool MoveZDown(const std::string& zAxis, double target, double slowAt, std::stop_token ct);

        /// <summary>Nâng Z theo hai mức — gương của <see cref="MoveZDown"/>: chậm tới điểm giảm tốc rồi nhanh.</summary>
        bool MoveZUp(const std::string& zAxis, double target, double slowAt, std::stop_token ct);

        /// <summary>Di chuyển TƯƠNG ĐỐI (Inching) — nút Move −/Move + ở trang Manual. Cùng chốt chặn CanMove với
        /// <see cref="Move"/>.</summary>
        bool MoveInc(const std::string& axisName, double delta, double speed, std::stop_token ct);

        /// <summary>Chọn Nhanh/Chậm cho jog tay — CHUNG cho cả 3 trục. Về gốc/MoveAbs/Auto không đọc cờ này.</summary>
        bool JogFast() const { return m_jogFast.load(); }
        void SetJogFast(bool v) { m_jogFast = v; }

        /// <summary>Jog liên tục (không chặn). dir: +1 / −1 / 0 = dừng; speed ≤ 0 → tốc độ jog theo <see cref="JogFast"/>.</summary>
        void Jog(const std::string& axisName, int dir, double speed = 0);

        /// <summary>
        /// Nâng Z lên cao độ an toàn — điều kiện tiên quyết của mọi chuyển động ngang, nên có hàm riêng thay
        /// vì để mỗi routine tự viết lại. Chốt chặn tắt (<c>ZSafeInterlock = false</c>) thì không làm gì.
        /// </summary>
        bool EnsureZSafe(std::stop_token ct);

        /// <summary>
        /// Chạy tới một điểm teach: nâng Z an toàn trước, các trục không phải Z có trong điểm chạy song song,
        /// Z (nếu điểm có) hạ SAU CÙNG. Trục không có trong điểm đứng yên. Dùng cho nút "Chạy tới" ở trang
        /// Teaching và là khối cơ bản cho chu trình Auto sau này. point = nullptr → "Điểm teach chưa có toạ độ".
        /// </summary>
        bool MoveToPosition(const TeachPosition* point, std::stop_token ct, bool slow = false);

        void StopAll() { m_motion.StopAll(); }

    private:
        bool MoveHoldable(const std::string& axisName, double pos, double speed, std::stop_token ct, bool interlock, bool gentle = false);
        void MarkMoveEnded() { m_lastMoveEndedMs = m_clock.ElapsedMilliseconds(); }

        MotionConnect& m_motion;
        SafetyMonitor& m_safety;
        McStatus& m_status;
        std::function<std::shared_ptr<SystemData>()> m_sys;
        std::shared_ptr<IIoDevice> m_io;

        /// <summary>Đồng hồ ĐƠN ĐIỆU cho <see cref="WaitSettle"/> — đổi giờ hệ thống không được làm lệch
        /// phép chờ này.</summary>
        base::Stopwatch m_clock;
        std::atomic<long long> m_lastMoveEndedMs;

        std::mutex m_runLock;
        std::atomic<bool> m_runBit{ true };
        std::stop_source m_runCts;
        std::atomic<bool> m_jogFast{ false };
    };
}
