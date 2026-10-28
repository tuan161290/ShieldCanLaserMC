#pragma once
// Jog "giữ để chạy, nhả là dừng" dùng chung cho các trang Manual, Teaching, Vision Edit.
//
// Lệnh jog của trục EtherCAT CHẶN chừng 20 ms (xung New set-point), nên không gọi trên luồng UI; nhưng cũng
// không được ném lên luồng nền tuỳ ý: hai luồng nền khác nhau có thể làm lệnh DỪNG (nhả nút) chạy trước
// lệnh CHẠY (nhấn nút) và trục chạy mãi. Mọi lệnh tay ngắn (jog, dừng) vì vậy đi qua MỘT hàng đợi tuần tự.
#include <functional>
#include <string>

namespace ui
{
    /// <summary>Xếp một lệnh tay ngắn vào hàng đợi tuần tự (một luồng nền, đúng thứ tự gửi). Lệnh dài (về
    /// gốc, chạy tới điểm) KHÔNG đi qua đây — dùng RunBackground.</summary>
    void PostMotionCommand(std::function<void()> fn);

    /// <summary>
    /// Gắn hành vi jog cho một nút: nhấn = McFunction::Jog(axis, dir), nhả / kéo chuột ra ngoài / mất chuột =
    /// McFunction::Jog(axis, 0). Bắt cả sự kiện con trỏ mà Button đã xử lý (handledEventsToo), nên dùng được
    /// với Button thường.
    /// </summary>
    void AttachJog(winrt::Microsoft::UI::Xaml::Controls::Button const& button, std::string axisName, int dir);

    /// <summary>Dừng mọi trục đang được giữ nút jog — gọi khi trang bị ẩn hoặc có lớp phủ modal mở, phòng
    /// trường hợp nút bị che trước khi kịp nhả.</summary>
    void StopActiveJogs();
}
