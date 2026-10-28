#pragma once
#include <array>
#include <string_view>

namespace scs
{
    /// <summary>
    /// Tên 3 trục dùng thống nhất toàn hệ thống: khoá tra cứu cấu hình, nguồn phát alarm, khoá của
    /// MotionConnect, và (qua <see cref="IndexOf"/>) chỉ số tính mã alarm trục. Dùng hằng số thay
    /// cho chuỗi rời rạc để gõ sai tên trục là lỗi biên dịch chứ không phải một alarm im lặng.
    ///
    /// <para>Máy Laser Marking (PLAN-LASER.md §1): X và Z mang cụm 7 nozzle (một Z chung), Y mang 2 tray. Cả ba là servo
    /// Panasonic MINAS A6B (CiA402) trên cùng một bus SOEM; thứ tự dưới đây cũng là thứ tự slave MẶC ĐỊNH (X = slave 0,
    /// Y = 1, Z = 2, Remote IO = 3) — vị trí slave thật sửa được ở trang EtherCAT.</para>
    /// </summary>
    struct AxisId
    {
        static constexpr const char* X = "X";
        static constexpr const char* Y = "Y";
        static constexpr const char* Z = "Z";

        /// <summary>Thứ tự cố định — vòng lặp và thứ tự hiển thị.</summary>
        static constexpr std::array<const char*, 3> All = { "X", "Y", "Z" };

        /// <summary>Trục Z — về gốc trước tiên, đi sau cùng khi tới điểm teach, và phải ở cao độ an toàn thì X/Y mới
        /// được chạy (<c>MotionConnect::ZSafeOk</c>).</summary>
        static constexpr std::array<const char*, 1> ZAxes = { "Z" };

        /// <summary>Các trục trong mặt phẳng ngang — chỉ được chạy khi Z ở cao độ an toàn.</summary>
        static constexpr std::array<const char*, 2> PlanarAxes = { "X", "Y" };

        /// <summary>Chỉ số 0..2 theo thứ tự <see cref="All"/>; -1 nếu không phải tên trục hợp lệ.</summary>
        static int IndexOf(std::string_view name)
        {
            for (size_t i = 0; i < All.size(); ++i)
                if (name == All[i]) return (int)i;
            return -1;
        }

        static bool IsValid(std::string_view name) { return IndexOf(name) >= 0; }

        static bool IsZ(std::string_view name)
        {
            for (const char* z : ZAxes) if (name == z) return true;
            return false;
        }

        /// <summary>Ô của trục trong lưới hiển thị (trang Main, Vision Edit): một hàng X, Y, Z.</summary>
        static void GridSlot(std::string_view name, int& row, int& column)
        {
            row = 0;
            column = IndexOf(name) < 0 ? 0 : IndexOf(name);
        }
        static constexpr int GridRows = 1;
        static constexpr int GridColumns = 3;
    };
}
