#pragma once
#include "Data/SystemData.h"

#include <map>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>Nhóm biến toàn cục theo ai ghi. Từ 2026-09-13 chỉ còn một nhóm: App ghi tất cả.</summary>
    enum class EGlobalVarGroup
    {
        /// <summary>App ghi ngay trước mỗi lần gọi Flow.</summary>
        AppWrites = 0
    };

    /// <summary>Một biến toàn cục cần có trong solution.</summary>
    struct GlobalVarSpec
    {
        std::string Name;
        /// <summary><c>float</c>, <c>int</c> hoặc <c>string</c> — ba kiểu module Global Variable tạo được qua SDK.</summary>
        std::string Type;
        /// <summary>Giá trị đầu, viết theo InvariantCulture (dấu chấm thập phân).</summary>
        std::string InitialValue;
        std::string Remark;
        EGlobalVarGroup Group{ EGlobalVarGroup::AppWrites };
    };

    enum class EGlobalVarState { Existing = 0, WrongType = 1, Missing = 2, Created = 3, Failed = 4, InvalidName = 5 };

    struct GlobalVarSyncItem
    {
        GlobalVarSpec Spec;
        EGlobalVarState State{ EGlobalVarState::Existing };
        std::string Detail;
    };

    struct GlobalVarSyncReport
    {
        /// <summary>true khi đã thực sự thử tạo biến (không phải chỉ kiểm tra).</summary>
        bool Applied{ false };
        bool Saved{ false };
        /// <summary>Lỗi chặn cả lượt (chưa nạp solution, không đọc được danh sách biến, lưu hỏng...).</summary>
        std::string Error;
        std::vector<GlobalVarSyncItem> Items;

        int Count(EGlobalVarState state) const;

        static GlobalVarSyncReport Fail(const std::string& error);

        /// <summary>Bản tóm tắt để hiện trên trang Setting và ghi log; xuống dòng bằng "\r\n" như bản C#.</summary>
        std::string ToText() const;
    };

    /// <summary>Kho biến toàn cục của solution — VisionMaster thật hoặc bản giả lập.</summary>
    class IGlobalVarStore
    {
    public:
        virtual ~IGlobalVarStore() = default;

        /// <summary>Tên → kiểu của mọi biến đang khai. Không đọc được thì ném lỗi, KHÔNG trả rỗng.</summary>
        virtual std::map<std::string, std::string> ReadAll() = 0;

        /// <summary>Tạo biến với giá trị trung tính (0 hoặc chuỗi rỗng).</summary>
        virtual void Add(const std::string& name, const std::string& type, const std::string& remark) = 0;

        /// <summary>Ghi giá trị đầu (chuỗi InvariantCulture) bằng hàm ghi theo kiểu.</summary>
        virtual void SetValue(const std::string& name, const std::string& type, const std::string& invariantValue) = 0;

        /// <summary>Lưu solution, trả thông báo của SDK.</summary>
        virtual std::string Save() = 0;
    };

    /// <summary>
    /// Đối chiếu danh mục biến với solution và tạo biến còn thiếu (Tuấn yêu cầu 2026-09-13).
    ///
    /// <para><b>Chỉ thêm, không sửa.</b> Biến đã có giữ nguyên giá trị. Biến sai kiểu chỉ báo: đổi kiểu trong
    /// VisionMaster gỡ luôn các subscribe đang trỏ vào biến đó.</para>
    ///
    /// <para><b>Giá trị đầu ghi riêng.</b> Hàm tạo biến của VisionMaster đọc số theo culture Windows, máy dùng
    /// dấu phẩy thập phân sẽ từ chối <c>0.05</c>. Vì vậy biến được tạo với 0 rồi mới ghi giá trị thật bằng hàm
    /// ghi float/int — hai hàm này nhận số nhị phân, không qua chuỗi.</para>
    /// </summary>
    class GlobalVarSync
    {
    public:
        /// <summary>Luật đặt tên của module Global Variable (VariableManager.CheckVarName, VM 4.4).
        /// Trả false thì <paramref name="why"/> nói lý do.</summary>
        static bool IsValidName(const std::string& name, std::string& why);

        /// <summary>store = nullptr → báo "thiếu kho biến hoặc danh mục biến".</summary>
        static GlobalVarSyncReport Run(IGlobalVarStore* store, const std::vector<GlobalVarSpec>& specs, bool apply);
    };

    /// <summary>
    /// Danh mục biến toàn cục — 5 biến, <b>App ghi hết</b> trước mọi lần kích Flow (PLAN-LASER.md §4.1): X_mm, Z_mm,
    /// Nozzle, RunId (float) và IsLocate (int). Hằng số hiệu chuẩn nằm ở model của App, solution không cất bản sao —
    /// nên giá trị đầu của mọi biến đều là 0 (IsLocate = 1).
    /// </summary>
    class VisionGlobalVarCatalog
    {
    public:
        /// <summary>sys = nullptr → dùng bộ tên mặc định.</summary>
        static std::vector<GlobalVarSpec> Build(const SystemData* sys);
        static std::vector<GlobalVarSpec> Build(const VisionOutputNames& names);
    };
}
