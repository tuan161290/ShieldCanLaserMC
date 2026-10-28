#pragma once
#include <cmath>

namespace scs
{
    /// <summary>
    /// Số học góc cho trục quay tự do — quấn toạ độ về <b>[0, 360)</b> và luôn đi đường ngắn.
    /// Trục quay không phải trục số thẳng: 350° và 10° chỉ cách nhau 20°, và cùng một tư thế có vô
    /// số con số biểu diễn (10, 370, −350…). Gom về một chỗ để không nơi nào quên xử lý.
    ///
    /// <para><b>Quy ước hoà:</b> đúng nửa vòng (±180°) thì đi chiều DƯƠNG — phải chốt một chiều cố
    /// định, không thì cùng một lệnh có thể quay hai hướng khác nhau giữa hai lần chạy.</para>
    /// </summary>
    struct AxisAngle
    {
        static constexpr double FullTurn = 360.0;

        /// <summary>Quấn về [0, 360): −10 → 350, 370 → 10, 360 → 0. NaN/vô cực trả 0 thay vì lan ra
        /// ngoài: giá trị hỏng lọt tới lệnh chạy sẽ thành một lệnh tới toạ độ vô nghĩa.</summary>
        static double Wrap(double deg)
        {
            if (std::isnan(deg) || std::isinf(deg)) return 0.0;

            double w = std::fmod(deg, FullTurn);
            if (w < 0) w += FullTurn;

            // Góc âm rất nhỏ (−0.0000001°, luôn xuất hiện vì trục dừng quanh đích) quấn ra 359.9999999
            // — cùng tư thế với 0° nhưng đọc như cách gần trọn vòng. Kéo về 0 hẳn. Ngưỡng 1e-6 độ nhỏ
            // hơn một xung (0.018°) bốn bậc độ lớn nên không gộp nhầm hai tư thế phân biệt được.
            if (FullTurn - w < 1e-6) w = 0.0;
            return w;
        }

        /// <summary>Góc ngắn nhất đi từ fromDeg tới toDeg, trong (−180, +180]. Dương = chiều tăng toạ độ.</summary>
        static double ShortestDelta(double fromDeg, double toDeg)
        {
            double d = Wrap(toDeg - fromDeg);
            return d > 180.0 ? d - FullTurn : d;
        }

        /// <summary>Khoảng cách góc, [0, 180] — dùng thay abs(a − b) khi so hai góc.</summary>
        static double Distance(double aDeg, double bDeg)
        {
            return std::fabs(ShortestDelta(aDeg, bDeg));
        }
    };
}
