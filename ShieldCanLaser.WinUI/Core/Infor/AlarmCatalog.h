#pragma once
#include "Infor/Enums.h"

#include <string>

namespace scs
{
    /// <summary>Nội dung tĩnh của một mã alarm, nạp từ Config\alarms.json (khoá JSON viết thường:
    /// level, vi, en, cause, remedy, causeEn, remedyEn; level ghi dạng chuỗi "Error", đọc nhận cả chuỗi lẫn số).
    /// causeEn / remedyEn là khoá tuỳ chọn (2026-10-18) — file cũ không có thì được bổ sung từ bản mẫu.</summary>
    struct AlarmDefinition
    {
        EAlarmLevel Level{ EAlarmLevel::Error };
        std::string Vi;
        std::string En;
        std::string Cause;
        std::string Remedy;
        std::string CauseEn;
        std::string RemedyEn;
    };

    /// <summary>
    /// Tra cứu nội dung alarm. Nạp từ <c>%Root%\Config\alarms.json</c>; nếu chưa có thì lấy bản
    /// mẫu cạnh exe và chép sang Config để người dùng sửa được lời văn mà không cần build lại.
    ///
    /// <para><b>Ai sở hữu cái gì.</b> Lời văn (vi, en, cause, remedy) thuộc về người dùng — phần
    /// mềm không bao giờ ghi đè (chỉ điền vào ô en / causeEn / remedyEn còn TRỐNG từ bản mẫu). Còn <c>level</c> quyết định máy có dừng hay không, nên nó thuộc về
    /// phần mềm và luôn được lấy lại theo bản mẫu: nếu mức nằm trong tay file cấu hình thì một lần
    /// sửa nhầm trong Notepad là đủ hạ một lỗi Fatal xuống Warning và máy chạy tiếp qua sự cố.</para>
    /// </summary>
    class AlarmCatalog
    {
    public:
        static bool Loaded();
        static void Load();

        /// <summary>
        /// Tra định nghĩa của một mã. Mã trục (2000–2899) rơi về mẫu <c>axisFaults</c> nếu không có
        /// bản ghi riêng ({AXIS} được thay bằng tên trục). Không tra được thì trả về định nghĩa
        /// "chưa định nghĩa" — một mã thiếu trong bảng vẫn phải hiện lên màn hình.
        /// </summary>
        static AlarmDefinition Get(int code);
    };
}
