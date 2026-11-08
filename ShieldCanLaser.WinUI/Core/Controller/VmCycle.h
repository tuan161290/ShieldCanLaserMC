#pragma once
#include <limits>
#include <string>

#include "Communication/VmContract.h"
#include "Data/SystemData.h"

namespace scs
{
    /// <summary>Kết quả đọc một lần kiểm tra (<see cref="VmCycle::ReadInspect"/>).</summary>
    struct VmInspectRead
    {
        bool CallOk{ false };
        std::string CallError;
        /// <summary>Output <c>Result</c> đọc được thành số hữu hạn.</summary>
        bool HasResult{ false };
        double Result{ std::numeric_limits<double>::quiet_NaN() };
        /// <summary>Result = 1 → can đạt. Khác 1 (0, −1…) → NG.</summary>
        bool Pass{ false };

        /// <summary>Một dòng cho log và trang: "OK (Result 1)", "NG (Result 0)" hoặc lý do không đọc được.</summary>
        std::string Describe() const;
    };

    /// <summary>
    /// Hợp đồng gọi Flow VisionMaster của máy Laser Marking: vision chỉ KIỂM TRA sau khi khắc (PLAN-LASER.md §4). Mỗi nozzle
    /// một lần gọi — chạy Flow của <c>VisionOutputNames::InspectPath</c>, đọc output <c>Result</c> của Group đó.
    /// <para>Mọi hàm là hàm thuần (không chặn), gọi được từ mọi luồng.</para>
    /// </summary>
    class VmCycle
    {
    public:
        /// <summary>Lời gọi kiểm tra can trên nozzle <paramref name="nozzle"/> (1…7, chỉ để gắn nhãn ảnh).</summary>
        static VmCallRequest InspectRequest(const VisionOutputNames& n, int nozzle);

        static VmInspectRead ReadInspect(const VmCallResult& r);

        /// <summary>Số gọn cho log: tối đa 3 chữ số lẻ, bỏ số 0 thừa; NaN/vô cực ghi rõ.</summary>
        static std::string Fmt(double v);
    };
}
