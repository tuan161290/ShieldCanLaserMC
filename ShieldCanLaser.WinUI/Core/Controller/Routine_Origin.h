#pragma once
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "Communication/MotionConnect.h"
#include "Controller/McFunction.h"
#include "Controller/McStatus.h"
#include "Data/SystemData.h"

namespace scs
{
    /// <summary>
    /// Chuỗi về gốc, theo nguyên tắc "nâng lên trước, tách ra sau" của ShieldCanAssy: Z về gốc
    /// trước (O2), chốt chặn đọc LẠI vị trí thực của Z (O3), rồi X/Y song song (O5), cuối cùng mọi trục về vị trí chờ (O6).
    ///
    /// <para>Thứ tự này giả định Z (cụm 7 nozzle) là trục duy nhất có thể va khi X/Y chạy ngang — CHƯA xác nhận với cơ
    /// khí máy Laser Marking.</para>
    /// </summary>
    class Routine_Origin
    {
    public:
        Routine_Origin(MotionConnect& motion, McFunction& func, McStatus& status, std::function<std::shared_ptr<SystemData>()> sys);

        /// <summary>Chạy cả chuỗi về gốc (HÀM CHẶN, luồng nền). Không ném: huỷ/lỗi đều trả false sau khi đã
        /// dừng trục, đặt OrgOk = false và OriginStep = Aborted.</summary>
        bool Run(std::stop_token ct);

        /// <summary>
        /// Init (Tuấn 2026-09-29): đưa máy về vị trí chờ mà KHÔNG về gốc lại — nâng Z lên cao độ an toàn trước, rồi mọi trục
        /// về StandbyPos như bước cuối của Về gốc (O6). Dùng sau khi tạm dừng, chuyển MANUAL và thao tác tay, trước khi chạy tiếp.
        /// HÀM CHẶN, không ném.
        /// </summary>
        bool Init(std::stop_token ct);

    private:
        bool PreCheck();
        bool Fail(const std::string& reason);
        bool ServoOnAll(std::stop_token ct);
        bool HomeParallel(const std::vector<std::string>& names, std::stop_token ct);
        bool HomeSequential(const std::vector<std::string>& names, std::stop_token ct);
        bool GateZSafe();
        bool MoveStandby(std::stop_token ct);
        bool MoveStandbyOne(const std::shared_ptr<IAxis>& axis, std::stop_token ct);
        void Step(EOriginState step);
        bool Abort(const std::string& reason);

        MotionConnect& m_motion;
        McFunction& m_func;
        McStatus& m_status;
        std::function<std::shared_ptr<SystemData>()> m_sys;
    };
}
