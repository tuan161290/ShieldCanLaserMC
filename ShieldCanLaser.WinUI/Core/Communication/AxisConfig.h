#pragma once
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace scs
{
    /// <summary>
    /// Cấu hình một trục, lưu trong system.json. Cả 3 trục là servo CiA402 trên cùng một bus EtherCAT
    /// nên chỉ có MỘT bộ tham số phần cứng. Tách khỏi <c>ecm::AxisConfig</c> để tầng Controller và UI
    /// không phải biết kiểu dữ liệu của thư viện EtherCAT. Đối tượng được chia sẻ qua shared_ptr
    /// (SystemData.Axes ↔ IAxis.Config): sửa ở trang Setting có tác dụng ngay trên trục.
    /// </summary>
    struct AxisConfig
    {
        std::string Name;

        /// <summary>"mm" cho trục tịnh tiến, "deg" cho trục xoay.</summary>
        std::string Unit = "mm";

        // ── EtherCAT ──
        /// <summary>Vị trí trên chuỗi EtherCAT (0-based, theo thứ tự đi dây).</summary>
        int SlavePosition = 0;

        /// <summary>Trục thứ mấy của driver: 0 với driver một trục; driver hai trục Welcon WE2x (một slave chạy
        /// máy Slit cũ) có hai trục cùng SlavePosition, khác kênh. Máy Laser: mỗi driver A6B một trục, kênh 0.</summary>
        int AxisChannel = 0;

        /// <summary>Counts của driver trên một đơn vị (mm/độ của TẢI) — <b>hằng số theo thiết kế</b>.
        /// Sai số này trục vẫn chạy, vẫn báo tới nơi, chỉ là tới sai chỗ.</summary>
        double CountsPerUnit = 1000.0;

        /// <summary>Homing method CiA402 (0x6098) — chiều và kiểu về gốc do driver thực hiện.</summary>
        int8_t HomingMethod = 1;

        /// <summary>Tốc độ về gốc, đơn vị/giây (0x6099:1/:2).</summary>
        double HomingVelocity = 10.0;

        /// <summary>Thời gian tăng/giảm tốc từ 0 tới tốc độ lệnh, giây (trang EtherCAT nhập theo ms). Quá ngắn:
        /// rung, quá dòng.</summary>
        double AccelTimeS = 0.2;
        double DecelTimeS = 0.2;

        /// <summary>Hành trình trên một vòng động cơ — ghi vào 0x6092:1 lúc Connect All Axis.
        /// nullopt = không ghi, giữ nguyên giá trị trên driver.</summary>
        std::optional<double> TravelPerRotation;

        /// <summary>Xung (counts) trên một vòng động cơ. Chỉ dùng để tính <see cref="CountsPerUnit"/>
        /// (<see cref="RecomputeCountsPerUnit"/>), không ghi xuống driver. nullopt = chưa nhập.</summary>
        std::optional<double> PulsesPerRotation;

        /// <summary>CountsPerUnit = PulsesPerRotation ÷ TravelPerRotation khi đã có cả hai — như EtherCATMaster
        /// ConfigApp (vd. 10000 xung/vòng, vít me 10 mm → 1000 counts/mm). Gọi khi một trong hai số đổi;
        /// CountsPerUnit vẫn gõ tay được, giá trị gõ tay giữ tới lần một trong hai số đổi tiếp.</summary>
        void RecomputeCountsPerUnit()
        {
            if (PulsesPerRotation && TravelPerRotation && *TravelPerRotation != 0)
                CountsPerUnit = *PulsesPerRotation / *TravelPerRotation;
        }

        /// <summary>Ghi giới hạn mềm xuống driver (0x607D) lúc Connect All Axis. Tắt với trục quấn vòng.</summary>
        bool WriteSoftLimitToDrive = true;

        // ── Định danh driver — ConnectAllAxis so khớp slave trên bus với các số này ──
        uint32_t ExpectedVendorId = 0;      // 0x1018:1; 0 = slot chưa gán driver
        uint32_t ExpectedProductCode = 0;   // 0x1018:2; 0 = slot chưa gán driver
        uint32_t ExpectedRevision = 0;      // 0x1018:3 — chỉ để hiển thị
        std::string ExpectedDriverName;

        // Hai hàm dưới không ghi vào JSON ([JsonIgnore]).
        bool IsConfigured() const { return ExpectedProductCode != 0; }
        std::string DriverIdText() const
        {
            return IsConfigured() ? std::format("0x{:X}/0x{:X}", ExpectedVendorId, ExpectedProductCode) : "(unassigned)";
        }

        // ── Giới hạn và tốc độ ──
        double SoftLimitPos = 500.0;
        double SoftLimitNeg = -1.0;

        /// <summary>Dung sai "đã tới nơi". Phải nhỏ hơn hẳn bước Inching nhỏ nhất — nếu bước ≤ dung
        /// sai, lệnh có thể báo xong ngay mà trục chưa nhúc nhích (bài học 2026-09-09).</summary>
        double InPositionTolerance = 0.01;

        double JogSpeedSlow = 10.0;
        double JogSpeedFast = 20.0;

        /// <summary>Ba mức tốc độ chạy tự động. MoveSpeed là mức CAO; Mid/Low suy ra 50%/20% khi chưa
        /// đặt (≤ 0). JSON ghi giá trị ĐÃ SUY RA (như Newtonsoft đọc getter).</summary>
        double MoveSpeed = 100.0;

        double MoveSpeedMid() const { return m_moveSpeedMid > 0 ? m_moveSpeedMid : MoveSpeed * 0.5; }
        void SetMoveSpeedMid(double v) { m_moveSpeedMid = v; }
        double MoveSpeedLow() const { return m_moveSpeedLow > 0 ? m_moveSpeedLow : MoveSpeed * 0.2; }
        void SetMoveSpeedLow(double v) { m_moveSpeedLow = v; }

        // ── Về gốc ──
        /// <summary>Chiều về gốc dự kiến: +1/−1 — chỉ mô tả cho UI/trục mô phỏng.</summary>
        int HomeDir = -1;
        int HomeTimeoutMs = 60000;
        int MoveTimeoutMs = 30000;

        /// <summary>Toạ độ ứng với vị trí gốc — trục mô phỏng dùng làm đích về gốc.</summary>
        double HomeOffset = 0;

        /// <summary>Vị trí chờ sau khi về gốc xong (bước O6) và đích của nút Init.</summary>
        double StandbyPos = 0;

        /// <summary>Trục quay tự do, toạ độ quấn về [0, 360): vị trí đọc về được quấn, lệnh tuyệt đối
        /// đi đường ngắn, InSoftLimit luôn đúng. Mặc định TẮT cho mọi trục.</summary>
        bool WrapDegrees = false;

        bool InSoftLimit(double pos) const { return WrapDegrees || (pos >= SoftLimitNeg && pos <= SoftLimitPos); }

    private:
        double m_moveSpeedMid = 0;
        double m_moveSpeedLow = 0;
    };
}
