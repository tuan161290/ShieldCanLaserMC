#pragma once
#include "Utility/DateTimeText.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scs
{
    /// <summary>
    /// Một điểm teach: tên + vị trí của những trục tham gia. Trục không có trong <see cref="Pos"/> là
    /// trục điểm này không đụng tới — "Chạy tới" chỉ chạy các trục có giá trị.
    /// </summary>
    struct TeachPosition
    {
        std::string Name = "Point";
        std::string Note;

        /// <summary>Tên trục (AxisId) → toạ độ máy. Giữ THỨ TỰ CHÈN như Dictionary của .NET; sửa qua
        /// Set/Remove (thay cho Pos[axis] = v / Pos.Remove).</summary>
        std::vector<std::pair<std::string, double>> Pos;

        std::optional<TimePoint> TaughtAt;

        bool Taught() const { return TaughtAt.has_value() && !Pos.empty(); }

        bool Has(std::string_view axis) const;
        double Get(std::string_view axis, double fallback = 0) const;
        void Set(std::string_view axis, double value);
        bool Remove(std::string_view axis);
    };

    /// <summary>
    /// Tham số chu trình Auto của model (JSON khoá "Auto") — PLAN-LASER.md §2. Số mặc định là PHỎNG ĐOÁN để chạy mô
    /// phỏng — chỉnh lại trên máy thật. Toạ độ trục không nằm ở đây mà ở các điểm teach (<see cref="AutoPoints"/>).
    /// </summary>
    struct AutoParams
    {
        /// <summary>Số hàng mỗi tray (2 tray trên Y). Số cột cố định = 7 nozzle — gắp cả hàng một lượt.</summary>
        int TrayRows = 7;
        /// <summary>Chờ hút chắc sau khi hạ Z (ms) trước khi nhấc.</summary>
        int VacuumOnMs = 150;
        /// <summary>Chờ nhả hẳn sau khi tắt hút (ms) trước khi nhấc.</summary>
        int VacuumOffMs = 150;
        /// <summary>Đoạn cuối (mm) trước cao độ hút/trả Z chạy ở tốc độ THẤP.</summary>
        double PickSlowLength = 2.0;
        /// <summary>Bật bước CAM. Tắt = khắc đúng điểm teach, không bù.</summary>
        bool UseVision = true;
        /// <summary>Bật kích laser. Tắt = chạy khô: đi đủ các điểm, không kích DO.</summary>
        bool UseLaser = true;
        /// <summary>Giữ DO kích laser chừng này ms rồi coi là khắc xong — theo nội dung khắc của model (laser không báo xong).</summary>
        int MarkTimeMs = 700;
    };

    /// <summary>
    /// Tên điểm teach mà chu trình Auto dùng — model mới có sẵn đủ, model cũ được bổ sung lúc nạp
    /// (<see cref="ModelData::EnsureAutoPoints"/>). Đổi tên thì chu trình không tìm thấy điểm.
    ///
    /// <para>Mỗi nhóm chỉ teach ĐẦU và CUỐI, các điểm giữa chia đều (<see cref="ModelData::Interpolate"/>): hàng tray
    /// (nozzle 1 trên cột 1 của hàng 1 / hàng cuối, mỗi tray), vị trí CAM (nozzle 1 / 7 giữa khung), vị trí khắc
    /// (nozzle 1 / 7 ở tâm trường laser). Vị trí chờ = StandbyPos từng trục (Setting).</para>
    /// </summary>
    struct AutoPoints
    {
        static constexpr int TrayCount = 2;
        static constexpr int NozzleCount = 7;

        /// <summary>Nozzle 1 đúng trên cột 1 của hàng 1 / hàng cuối (<paramref name="last"/>) của tray <paramref name="tray"/> (1, 2): X, Y, Z (Z = cao độ hút).</summary>
        static std::string PickRow(int tray, bool last);
        /// <summary>Nozzle 1 / nozzle 7 (<paramref name="last"/>) đúng giữa khung CAM: X, Z.</summary>
        static std::string Cam(bool last);
        /// <summary>Nozzle 1 / nozzle 7 (<paramref name="last"/>) đúng tâm trường laser: X, Z.</summary>
        static std::string Laser(bool last);

        /// <summary>Điểm thuộc các tab Pick / CAM / Laser của trang Teaching (không hiện ở "Other").</summary>
        static bool IsLayoutPoint(std::string_view name);

        /// <summary>Mọi điểm, theo thứ tự hiển thị: (tên, ghi chú).</summary>
        static std::vector<std::pair<std::string, std::string>> All();
    };

    /// <summary>
    /// Bản ghi hiệu chuẩn của MỘT nozzle — App đo rồi ghi vào đây (PLAN-LASER.md §4.1, §4.2). <c>Ref</c> là
    /// <c>Rel = (ObjX − X_mm, ObjY − Z_mm)</c> đo lúc "SET MASTER" với can mẫu lắp chuẩn; chu trình lấy độ lệch = Rel
    /// đo được − Ref. <c>Trim</c> cộng thẳng vào điểm khắc (mm), tối đa ±1 mm.
    /// </summary>
    struct NozzleCalib
    {
        double RefX = 0, RefZ = 0;
        std::optional<TimePoint> RefAt;
        double TrimX = 0, TrimZ = 0;

        bool RefMeasured() const { return RefAt.has_value(); }
    };

    /// <summary>Bản ghi hiệu chuẩn vision của model (JSON khoá "Calib"): kiểm tra lưới dùng chung + 7 nozzle.</summary>
    struct VisionCalib
    {
        static constexpr int NozzleCount = 7;
        static constexpr double MaxTrimMm = 1.0;

        /// <summary>Kiểm tra lưới X × Z đạt lúc nào; nullopt = chưa đạt.</summary>
        std::optional<TimePoint> GridVerifiedAt;
        std::array<NozzleCalib, NozzleCount> Nozzles;

        /// <summary>Nozzle 1…7 (ngoài khoảng thì kẹp về 1 hoặc 7).</summary>
        NozzleCalib& Nozzle(int k) { return Nozzles[ClampIndex(k)]; }
        const NozzleCalib& Nozzle(int k) const { return Nozzles[ClampIndex(k)]; }
        bool GridVerified() const { return GridVerifiedAt.has_value(); }

    private:
        static size_t ClampIndex(int k) { return static_cast<size_t>(k < 1 ? 0 : k > NozzleCount ? NozzleCount - 1 : k - 1); }
    };

    /// <summary>
    /// Dữ liệu một model sản phẩm — lưu ở Model\&lt;tên&gt;.json: danh sách điểm teach, tham số chu trình Auto và bản
    /// ghi hiệu chuẩn vision.
    /// </summary>
    struct ModelData
    {
        std::string Name = "DEFAULT";
        std::string Description;
        TimePoint ModifiedAt = std::chrono::system_clock::now();

        std::vector<std::shared_ptr<TeachPosition>> TeachPositions;

        AutoParams Auto;
        VisionCalib Calib;

        /// <summary>Thêm các điểm teach của chu trình Auto còn thiếu. Trả về số điểm đã thêm.</summary>
        int EnsureAutoPoints();

        /// <summary>Tìm theo tên, không phân biệt hoa thường; nullptr nếu không có.</summary>
        std::shared_ptr<TeachPosition> FindPosition(std::string_view name) const;

        // ── Điểm chia đều giữa điểm đầu và điểm cuối — bản sao mới mỗi lần gọi; nullptr + why nếu thiếu teach ──
        /// <summary>Nozzle 1 trên cột 1 của hàng <paramref name="row"/> (1…TrayRows) tray <paramref name="tray"/> (1, 2): X, Y, Z.</summary>
        std::shared_ptr<TeachPosition> PickPoint(int tray, int row, std::string& why) const;
        /// <summary>Nozzle <paramref name="nozzle"/> (1…7) giữa khung CAM: X, Z.</summary>
        std::shared_ptr<TeachPosition> CamPoint(int nozzle, std::string& why) const;
        /// <summary>Nozzle <paramref name="nozzle"/> (1…7) ở tâm trường laser: X, Z — CHƯA cộng Trim (Routine_Auto cộng).</summary>
        std::shared_ptr<TeachPosition> LaserPoint(int nozzle, std::string& why) const;

        /// <summary>Điểm thứ <paramref name="index"/> (0…count−1) của <paramref name="count"/> điểm cách đều từ
        /// <paramref name="first"/> tới <paramref name="last"/>. Trục có ở cả hai điểm thì nội suy; trục chỉ có ở điểm
        /// đầu thì giữ số của điểm đầu.</summary>
        static TeachPosition Interpolate(const TeachPosition& first, const TeachPosition& last, int index, int count);

        /// <summary>Model mới: đủ điểm teach của chu trình Auto (chưa có toạ độ).</summary>
        static std::shared_ptr<ModelData> CreateDefault(std::string_view name = "DEFAULT");

        std::string ToJsonText() const;
        static std::shared_ptr<ModelData> FromJsonText(const std::string& text);
    };
}
