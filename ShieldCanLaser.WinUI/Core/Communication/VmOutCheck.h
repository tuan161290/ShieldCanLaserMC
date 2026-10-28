#pragma once
#include "Data/SystemData.h"

#include <optional>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>Một output mà Flow <b>đang khai</b> trong solution — đọc thẳng từ tab Output Params qua SDK
    /// (<c>ProcedureResult.GetAllOutputNameInfo</c>), không phải thứ App mong đợi.</summary>
    struct VmDeclaredOut
    {
        std::string Name;
        /// <summary>Mã kiểu của SDK (<c>IMVS_MODULE_BASE_DATA_TYPE</c>) — giữ nguyên số để lớp trên không
        /// phải tham chiếu SDK VisionMaster.</summary>
        int TypeCode{ 0 };
        /// <summary>Tên kiểu cho người đọc: "float", "int", hoặc mã thô nếu là kiểu khác.</summary>
        std::string TypeText;
        /// <summary>App đọc được thành số hay không. Kiểu khác thì Flow vẫn chạy, chỉ có điều App báo
        /// "thiếu output" y như khi output không tồn tại.</summary>
        bool Numeric{ false };
    };

    /// <summary>Một dòng trong tab Input Params / Output Params của một MODULE bên trong Flow.</summary>
    struct VmIoInfo
    {
        std::string Name;
        /// <summary>Tên kiểu VisionMaster hiện ở cột Type.</summary>
        std::string TypeText;
        /// <summary>Ô Subscribe trỏ vào đâu. RỖNG nghĩa là chưa bind — với Input Params của Script thì
        /// đây là lỗi im lặng nguy hiểm nhất: Script vẫn biên dịch, số vào luôn bằng 0.</summary>
        std::string Source;

        bool Bound() const { return !Source.empty(); }
    };

    /// <summary>Một module bên trong Flow, kèm hai tab tham số của nó.</summary>
    struct VmModuleIo
    {
        std::string Name;
        std::vector<VmIoInfo> Inputs;
        std::vector<VmIoInfo> Outputs;
    };

    /// <summary>Những gì đọc được của MỘT Flow. <see cref="Error"/> khác rỗng nghĩa là không đọc được gì.</summary>
    struct VmFlowOutputs
    {
        std::string Flow;
        std::string Error;
        std::vector<VmDeclaredOut> Declared;

        /// <summary>Số Input Params Flow còn khai. −1 = SDK không trả lời được.</summary>
        int InputParamCount{ -1 };

        /// <summary>Các module bên trong Flow (kể cả module nằm trong Group). Rỗng nghĩa là SDK không
        /// duyệt được — phần kiểm Script sẽ nói thẳng thay vì báo thiếu sạch.</summary>
        std::vector<VmModuleIo> Modules;

        static VmFlowOutputs Fail(const std::string& flow, const std::string& why);

        /// <summary>Thêm một output cấp Flow từ (tên, mã kiểu SDK): tự điền TypeText/Numeric đúng như
        /// VisionConnect bản C# làm — thiết bị thật gọi hàm này khi dựng kết quả ReadFlowOutputs.</summary>
        void AddDeclared(const std::string& name, int typeCode);
    };

    /// <summary>Bộ output mà hợp đồng đòi ở một Flow — dựng từ chính các hàm tạo lời gọi của
    /// <c>VmCycle</c> nên không thể lệch với thứ App thật sự đọc lúc chạy.</summary>
    struct VmFlowExpect
    {
        std::string Flow;
        /// <summary>Mọi tên phải có trong tab Output Params. Ở đây KHÔNG chia bắt buộc / tuỳ chọn như
        /// lúc chạy: <c>TopX_mm</c> được phép rỗng khi không thấy vật, nhưng <b>khai thiếu</b> nó thì
        /// vẫn là lỗi dựng solution.</summary>
        std::vector<std::string> Names;
        /// <summary>Tên output ảnh — có thì tốt (trang Main hiện ảnh), thiếu không phải lỗi.</summary>
        std::string ImageName;
        /// <summary>Một câu nhắc riêng của Flow này, in kèm khi có chuyện.</summary>
        std::string Note;
    };

    /// <summary>
    /// Hai tab tham số mà một module <b>Script</b> phải khai — theo cẩm nang v14.
    ///
    /// <para><b>Tên ở tầng này là tự do.</b> Script gọi biến bằng đúng tên khai ở hai tab; gõ lệch giữa tab và
    /// mã thì VisionMaster báo <b>lỗi biên dịch</b>, thấy ngay. Vì vậy phần đối chiếu tên ở đây là "khác cẩm
    /// nang", không phải "sai". Thứ thật sự im lặng là <b>ô Subscribe bỏ trống</b>: Script biên dịch trót
    /// lọt, số vào bằng 0, không có lỗi nào hiện ra ở đâu cả. Đó là thứ chính của phép kiểm này.</para>
    /// </summary>
    struct VmScriptExpect
    {
        std::string Flow;
        /// <summary>Tên module Script trong cẩm nang — chỉ để gọi tên trong báo cáo.</summary>
        std::string ScriptName;
        std::vector<std::string> Inputs;
        std::vector<std::string> Outputs;
    };

    /// <summary>Hợp đồng của Script <c>CanAbs</c> trong LocateCan: đọc toạ độ vật lý của can sau Calibration Transformation và
    /// echo, xuất ObjFound / ObjX_mm / ObjY_mm / ObjRunId.</summary>
    struct VmScriptContract
    {
        static const std::vector<std::string>& CanAbsIn();
        static const std::vector<std::string>& CanAbsOut();

        static std::vector<VmScriptExpect> Build(const VisionOutputNames& n);
    };

    enum class EVmOutState
    {
        /// <summary>Có, đúng kiểu số.</summary>
        Ok = 0,
        /// <summary>Hợp đồng đòi mà Flow không khai.</summary>
        Missing = 1,
        /// <summary>Có tên nhưng kiểu App không đọc được thành số.</summary>
        WrongType = 2,
        /// <summary>Flow khai một tên không có trong hợp đồng, mà lại rất giống một tên đang thiếu.</summary>
        Typo = 3,
        /// <summary>Flow khai thêm tên ngoài hợp đồng — App không đọc, không phải lỗi.</summary>
        Extra = 4,
        /// <summary>Input Params của Script khai rồi nhưng bỏ trống ô Subscribe.</summary>
        Unbound = 5
    };

    struct VmOutItem
    {
        std::string Flow;
        std::string Name;
        EVmOutState State{ EVmOutState::Ok };
        std::string TypeText;
        /// <summary>Với <see cref="EVmOutState::Typo"/>: tên đúng theo hợp đồng.</summary>
        std::string Suggest;
    };

    /// <summary>Kết quả cả lượt kiểm — bảy Flow.</summary>
    struct VmOutCheckReport
    {
        /// <summary>Lỗi chặn cả lượt (chưa nạp solution…).</summary>
        std::string Error;
        std::vector<VmOutItem> Items;
        /// <summary>Flow không đọc được: không có trong solution, hoặc SDK không trả danh sách.</summary>
        std::vector<std::string> FlowErrors;
        /// <summary>Chuyện đáng nói nhưng không phải lỗi (Input Params còn sót…).</summary>
        std::vector<std::string> Notes;
        /// <summary>Bảng Input/Output của từng Script, in nguyên khối ở cuối báo cáo.</summary>
        std::vector<std::string> ScriptBlocks;

        int Count(EVmOutState state) const;

        /// <summary>Không có gì phải sửa.</summary>
        bool Clean() const;

        static VmOutCheckReport Fail(const std::string& error);

        /// <summary>Báo cáo cho người đọc; xuống dòng bằng "\r\n" như bản C#.</summary>
        std::string ToText() const;
    };

    /// <summary>
    /// Đối chiếu tab <b>Output Params</b> của từng Flow với hợp đồng — phần tính toán thuần, không đụng SDK.
    ///
    /// <para>Vì sao cần: App chỉ đọc output <b>theo tên</b>, và tên không có thì SDK trả về rỗng y hệt như
    /// output khai sai kiểu. Lúc chạy thật, lỗi này hiện ra thành một câu "thiếu output X" ở giữa một bước
    /// hiệu chuẩn, mỗi lần một cái. Tool này đọc cả bảy Flow một lượt, lúc máy đang đứng yên, và nói hết
    /// những gì phải sửa.</para>
    /// </summary>
    struct VmOutCheck
    {
        /// <summary>Mã kiểu SDK App đọc được thành số (<c>IMVS_GRAP_TYPE_INT</c> / <c>_FLOAT</c>).</summary>
        static constexpr int TypeInt = 13;
        static constexpr int TypeFloat = 14;

        static std::string TypeName(int code);
        static bool IsNumeric(int code) { return code == TypeInt || code == TypeFloat; }

        /// <summary>Tên thật của một <c>VmIO</c> do người dùng khai bị bọc dấu phần trăm: IDE hiện
        /// <c>TopX_mm</c> nhưng SDK trả <c>%TopX_mm%</c>. Bóc ra để so với hợp đồng (Tuấn phát hiện 21/09).</summary>
        static std::string CleanIoName(const std::string& raw);

        /// <summary>Bỏ đuôi đơn vị và dấu gạch, hạ chữ thường — để <c>HandEyeDx</c> nhận ra là
        /// <c>HandEyeDx_mm</c> gõ thiếu đuôi.</summary>
        static std::string Normalize(const std::string& s);

        /// <summary>Khoảng cách sửa chữ (Levenshtein) — đủ để bắt kiểu gõ đảo chữ "Detal" ↔ "Delta".</summary>
        static int EditDistance(const std::string& a, const std::string& b);

        /// <summary>Tên đang thiếu nào giống <paramref name="declared"/> nhất — nullopt nếu không đủ giống.
        /// Chỉ so với những tên ĐANG THIẾU: nhờ vậy <c>TopX_mm</c> (cách <c>TopY_mm</c> đúng một chữ) không
        /// bao giờ bị đem ra gợi ý, vì bản thân nó nằm trong hợp đồng và đã khớp rồi.</summary>
        static std::optional<std::string> SuggestFor(const std::string& declared, const std::vector<std::string>& missing);

        /// <summary>Đối chiếu một Flow, ghi thẳng kết quả vào report. actual = nullptr → "không đọc được".</summary>
        static void CompareFlow(const VmFlowExpect& expect, const VmFlowOutputs* actual, VmOutCheckReport& report);

        /// <summary>Tìm module Script trong Flow: module khai được NHIỀU NHẤT các tên ở tab Output Params của
        /// hợp đồng. Không dò theo tên module vì tên đó người dùng đặt tuỳ ý.</summary>
        static const VmModuleIo* FindScript(const VmFlowOutputs* flow, const VmScriptExpect& expect);

        /// <summary>Đối chiếu hai tab tham số của một Script: thiếu / gõ sai tên / <b>chưa bind</b>, kèm một
        /// khối bảng liệt kê đủ Input để đọc một lượt.</summary>
        static void CompareScript(const VmScriptExpect& expect, const VmFlowOutputs* flow, VmOutCheckReport& report);
    };
}
