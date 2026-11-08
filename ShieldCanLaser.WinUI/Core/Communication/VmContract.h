#pragma once
#include <map>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>Camera của một Flow — máy Laser Marking chỉ có MỘT camera (CAM); None = Flow không có camera.</summary>
    enum class EVmCamera { None = 0, Cam = 1 };

    /// <summary>
    /// Một lần gọi Flow: chạy, đọc output số — trong CÙNG một lần giữ khoá lệnh của thiết bị. App không ghi biến toàn
    /// cục nào (vision chỉ kiểm tra sau khi khắc, PLAN-LASER.md §4).
    /// </summary>
    struct VmCallRequest
    {
        std::string Flow;
        /// <summary>Đường dẫn module chứa các output cần đọc ("Flow.Group"); rỗng = Output Params của chính Flow.</summary>
        std::string Module;
        EVmCamera Camera{ EVmCamera::None };
        /// <summary>Nozzle 1…7 đang trước CAM — chỉ để gắn nhãn ảnh trang Main; 0 = không rõ.</summary>
        int Nozzle{ 0 };

        /// <summary>Output bắt buộc — thiếu một cái là lần gọi thất bại.</summary>
        std::vector<std::string> Outputs;
    };

    struct VmCallResult
    {
        bool Ok{ false };
        bool TimedOut{ false };
        std::string Error;

        std::map<std::string, double> Values;

        bool Has(const std::string& name) const { return Values.find(name) != Values.end(); }

        /// <summary>Giá trị output; không có thì NaN để phép kiểm số hữu hạn bắt được.</summary>
        double Get(const std::string& name) const;

        static VmCallResult Fail(const std::string& why, bool timedOut = false);
    };

    struct VmMath
    {
        static bool IsFinite(double v);
        /// <summary>Số hữu hạn và làm tròn ra đúng <paramref name="b"/>.</summary>
        static bool SameInt(double a, int b);
    };
}
