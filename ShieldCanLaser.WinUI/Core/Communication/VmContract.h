#pragma once
#include <map>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>Camera của một Flow — máy Laser Marking chỉ có MỘT camera (CAM); None = Flow không có camera.</summary>
    enum class EVmCamera { None = 0, Cam = 1 };

    /// <summary>Một lần gọi Flow: ghi biến toàn cục, chạy, đọc output — trong CÙNG một lần giữ khoá lệnh của
    /// thiết bị. Không có Input Params: mọi số App gửi sang đều đi qua biến toàn cục.</summary>
    struct VmCallRequest
    {
        std::string Flow;
        EVmCamera Camera{ EVmCamera::None };

        /// <summary>Biến toàn cục ghi trước khi chạy. Ghi NGHIÊM: tên trống, tên chưa khai trong solution hoặc
        /// không đọc được danh sách biến đều là lỗi, không bỏ qua.</summary>
        std::map<std::string, double> Globals;

        /// <summary>Output bắt buộc — thiếu một cái là lần gọi thất bại.</summary>
        std::vector<std::string> Outputs;

        /// <summary>Output đọc nếu có. Tư thế khớp mẫu rỗng khi không thấy vật, nên nằm ở đây.</summary>
        std::vector<std::string> OptionalOutputs;
    };

    struct VmCallResult
    {
        bool Ok{ false };
        bool TimedOut{ false };
        std::string Error;

        std::map<std::string, double> Values;
        std::vector<std::string> MissingOptional;

        bool Has(const std::string& name) const { return Values.find(name) != Values.end(); }

        /// <summary>Giá trị output; không có thì NaN để phép kiểm số hữu hạn bắt được.</summary>
        double Get(const std::string& name) const;

        /// <summary>Output số nguyên (đi qua float). Không có hoặc không hữu hạn thì <c>INT_MIN</c> —
        /// không trùng mã hợp lệ nào.</summary>
        int GetInt(const std::string& name) const;

        static VmCallResult Fail(const std::string& why, bool timedOut = false);
    };

    struct VmMath
    {
        /// <summary>Dung sai so echo: số đi App → float32 → Script → float32 → App.</summary>
        static constexpr double EchoTol = 1e-3;

        static bool IsFinite(double v);
        static bool Near(double a, double b, double tol = EchoTol);
        static bool SameInt(double a, int b);
    };

    /// <summary>
    /// RunId của mỗi lần gọi Flow. Bắt đầu ngẫu nhiên để một Flow chạy lại từ phiên trước không trùng
    /// số; quay vòng về 1. Giữ dưới 10 000 000: nếu VisionMaster in số float bằng 7 chữ số khi Script đọc
    /// biến qua chuỗi thì số lớn hơn sẽ mất chữ số.
    /// </summary>
    struct VmRunIds
    {
        static constexpr int Max = 9999999;
        static int Next();
    };

    /// <summary>
    /// Biến toàn cục của hợp đồng — <b>App ghi tất cả</b> trước mỗi lần kích Flow (PLAN-LASER.md §4.1): ba biến trục
    /// <c>X_mm · Z_mm · Nozzle</c> (tên cấu hình ở trang Setting), <see cref="RunId"/> và biến rẽ nhánh
    /// <c>IsLocate</c> (int — 1 soi, 0 nạp điểm hiệu chuẩn; tên ở <c>VisionOutputNames::GvIsLocate</c>). Hằng số hiệu chuẩn
    /// (Ref từng nozzle) chỉ nằm ở model của App — solution không cất bản sao nào.
    /// </summary>
    struct VmVars
    {
        static constexpr const char* RunId = "RunId";
    };

    /// <summary>Tên output cấp Flow <c>LocateCan</c>. Tên có số đo mang đuôi đơn vị <c>_mm</c>. <c>ObjY_mm</c> là trục
    /// thứ hai của file hiệu chuẩn — ứng với trục Z của máy.</summary>
    struct VmOut
    {
        static constexpr const char* EchoRunId = "EchoRunId";
        static constexpr const char* EchoX = "EchoX_mm";
        static constexpr const char* EchoZ = "EchoZ_mm";

        // LocateCan — vị trí can (Script "CanAbs")
        static constexpr const char* ObjFound = "ObjFound";
        static constexpr const char* ObjX = "ObjX_mm";
        static constexpr const char* ObjY = "ObjY_mm";
        static constexpr const char* ObjRunId = "ObjRunId";

        /// <summary>Output của Echo0 — module đặt trước khớp mẫu nên luôn có, bắt buộc.</summary>
        static const std::vector<std::string>& LocateEcho();
        /// <summary>Output của Script — rỗng khi không thấy vật, tuỳ chọn.</summary>
        static const std::vector<std::string>& LocateScript();
    };

    /// <summary>Mã <c>Status</c> của phép kiểm lưới App tự phán (<c>VmCycle::JudgeGrid</c>).</summary>
    struct VmStatus
    {
        static constexpr int NoTarget = 0;
        static constexpr int Ok = 1;
        static constexpr int NoConclusion = 2;
        static constexpr int AngleOut = 3;
        static constexpr int EccOut = 4;
        static constexpr int Invalid = 6;
        static constexpr int AxisMoved = 7;
        static constexpr int Code8 = 8;

        /// <summary>Kiểm tra lưới X × Z — App tự phán từ ba lần đo (<c>VmCycle::JudgeGrid</c>).</summary>
        static std::string GridVerify(int st);
    };
}
