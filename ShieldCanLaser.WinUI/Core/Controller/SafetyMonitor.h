#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>

#include "Communication/IIoDevice.h"
#include "Communication/MotionConnect.h"
#include "Controller/McStatus.h"
#include "Data/SystemData.h"

namespace scs
{
    /// <summary>
    /// Giám sát an toàn — PLAN.md §3.3.
    ///
    /// <para>Chạy trong vòng điều khiển với chu kỳ ≤ 20 ms và có quyền ép trạng thái sang
    /// EMG/ALARM bất cứ lúc nào bằng cách huỷ <see cref="AbortToken"/>. Các routine
    /// <b>không</b> tự kiểm tra an toàn rải rác — chúng chỉ observe token này. Nhờ vậy chỉ có
    /// đúng một chỗ quyết định khi nào máy phải dừng, và thêm một điều kiện an toàn mới không
    /// phải đi sửa mọi routine.</para>
    ///
    /// <para>Nhắc lại: đây là lớp DỪNG CHU TRÌNH, không phải lớp bảo vệ. Việc cắt nguồn động lực
    /// do relay an toàn cứng + STO của driver đảm nhiệm.</para>
    ///
    /// <para>Chưa có điều kiện cửa: danh sách I/O máy Laser Marking chưa chốt — thêm vào đây (cùng
    /// <see cref="IoMap"/>) nếu máy có công tắc cửa.</para>
    /// </summary>
    class SafetyMonitor
    {
    public:
        SafetyMonitor(MotionConnect& motion, McStatus& status, std::function<std::shared_ptr<SystemData>()> sys);
        SafetyMonitor(const SafetyMonitor&) = delete;
        SafetyMonitor& operator=(const SafetyMonitor&) = delete;

        /// <summary>Gắn trước khi vòng quét chạy.</summary>
        void AttachIo(std::shared_ptr<IIoDevice> io) { m_io = std::move(io); }

        /// <summary>
        /// Gọi trên vòng quét đúng lúc rèm sáng vừa bị cắt, TRƯỚC khi <see cref="SafeToMove"/> tắt — McControl tạm dừng
        /// chu trình như STOP (tắt bit RUN), nên lệnh trục của chu trình đứng chờ chứ không báo lỗi. Xem <c>McControl::HoldForCurtain</c>.
        /// Đặt trước khi vòng quét chạy.
        /// </summary>
        void SetCurtainBroken(std::function<void()> fn) { m_curtainBroken = std::move(fn); }

        /// <summary>Token chung mà mọi routine phải observe. Bị huỷ ngay khi có điều kiện dừng.</summary>
        std::stop_token AbortToken() const;

        /// <summary>true khi mọi điều kiện an toàn đều đạt và không có alarm Error/Fatal.</summary>
        bool SafeToMove() const { return m_safeToMove.load(); }

        /// <summary>Lý do gần nhất khiến máy không được phép chạy — hiện thẳng lên UI.</summary>
        std::string BlockReason() const;

        /// <summary>Cấp token mới sau khi đã xử lý xong lần dừng trước.</summary>
        void RenewToken();

        /// <summary>Huỷ do người vận hành (bấm STOP khi đang về gốc), không phải do lỗi an toàn.</summary>
        void RequestAbort(const std::string& reason) { Abort(reason); }

        /// <summary>Một vòng đánh giá. Gọi từ vòng điều khiển của McControl.</summary>
        void Evaluate();

    private:
        void Abort(const std::string& reason);
        bool ComputeSafeToMove(bool ioOk, bool emgOk, bool curtainOk, bool airOk);
        void SetBlockReason(const char* reason);

        MotionConnect& m_motion;
        McStatus& m_status;
        std::function<std::shared_ptr<SystemData>()> m_sys;

        mutable std::mutex m_ctsLock;
        std::stop_source m_cts;

        std::shared_ptr<IIoDevice> m_io;
        std::function<void()> m_curtainBroken;
        bool m_curtainWasOk{ true };            // chỉ vòng quét đụng tới

        std::atomic<bool> m_safeToMove{ false };
        mutable std::mutex m_reasonMutex;
        std::string m_blockReason = "not initialized";
    };
}
