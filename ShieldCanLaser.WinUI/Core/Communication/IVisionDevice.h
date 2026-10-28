#pragma once
#include "Communication/GlobalVarSync.h"
#include "Communication/ImageData.h"
#include "Communication/VmContract.h"
#include "Communication/VmOutCheck.h"

#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>
    /// Kết quả một lần nạp điểm hiệu chuẩn (<see cref="IVisionDevice::TriggerCalib"/>): chỉ còn "thấy / không
    /// thấy". Mọi số đo của chu trình đi qua <see cref="IVisionDevice::CallFlow"/>.
    /// </summary>
    struct VisionPose
    {
        bool Found{ false };
        std::string Error;

        static VisionPose NotFound(const std::string& why = {})
        {
            VisionPose p;
            p.Found = false;
            p.Error = why;
            return p;
        }
    };

    /// <summary>Việc nạp điểm hiệu chuẩn — chạy trên CHÍNH Flow soi <c>LocateCan</c>, nhánh hiệu chuẩn (App ghi biến rẽ
    /// nhánh <c>IsLocate</c> = 0). Lần soi thì App gọi qua <see cref="IVisionDevice::CallFlow"/> với biến đó = 1.</summary>
    enum class EVisionProcedure
    {
        /// <summary>Lưới 9 điểm X × Z: mỗi lần chạy nạp thêm một điểm vào module N-point Calibration của LocateCan.</summary>
        GridCalib = 0
    };

    inline const char* ToString(EVisionProcedure p)
    {
        switch (p)
        {
        case EVisionProcedure::GridCalib: return "Grid calib";
        }
        return "?";
    }

    /// <summary>Kết quả "Clear Point" (<see cref="IVisionDevice::ClearCalibPoints"/>): rỗng lỗi là đã xoá danh sách điểm
    /// của mọi module hiệu chuẩn trong Flow; <see cref="Modules"/> là tên các module đã xoá.</summary>
    struct CalibClearResult
    {
        std::string Error;
        std::vector<std::string> Modules;
        bool Ok() const { return Error.empty(); }
        static CalibClearResult Fail(std::string why) { CalibClearResult r; r.Error = std::move(why); return r; }
    };

    /// <summary>
    /// Vị trí thật (đọc encoder) của các trục liên quan tới vision, gửi sang VisionMaster qua <b>biến toàn
    /// cục</b> ngay trước mỗi lần kích Flow hiệu chuẩn.
    ///
    /// <para>Module N-point Calibration trong solution đặt <i>Calibration Points Input = By Coordinate</i> rồi subscribe
    /// <i>Physical Coordinate X/Y</i> vào <c>X_mm</c> / <c>Z_mm</c>: camera cố định, can treo trên nozzle di chuyển theo
    /// cụm X–Z nên nạp điểm kèm toạ độ thật thì mọi quỹ đạo đều hợp lệ (Hikrobot "Calibration and Locating Theory" tr.18).</para>
    /// </summary>
    struct AxisSnapshot
    {
        double X{ 0 };
        double Z{ 0 };
        /// <summary>Nozzle 1…7 đang trước CAM; 0 = không rõ.</summary>
        int Nozzle{ 0 };
    };

    /// <summary>Lỗi do thiết bị vision/SDK báo: <c>what()</c> là thông báo lỗi của SDK. Thiết bị thật ném kiểu này;
    /// VisionConnect đổi nó thành kết quả thất bại kèm log.</summary>
    struct VisionDeviceError : std::runtime_error
    {
        explicit VisionDeviceError(const std::string& message, int code = -1) : std::runtime_error(message), Code(code) {}
        int Code;
    };

    /// <summary>Thiết bị vision. Mọi hàm là hàm CHẶN, gọi từ luồng nền; getter rẻ và an toàn đa luồng.</summary>
    class IVisionDevice
    {
    public:
        virtual ~IVisionDevice() = default;

        virtual bool IsConnected() const = 0;
        virtual bool IsSolutionLoaded() const = 0;

        virtual bool Connect() = 0;
        virtual void Disconnect() = 0;

        virtual bool LoadSolution(const std::string& solutionPath) = 0;

        /// <summary>
        /// Kích Flow <b>hiệu chuẩn</b>: ghi vị trí trục thật vào biến toàn cục của solution TRƯỚC, rồi mới
        /// chạy Flow — nên module hiệu chuẩn nạp điểm kèm đúng toạ độ vật lý của lần chụp đó.
        ///
        /// <para>Ghi biến và chạy Flow phải nằm trong CÙNG một lời gọi: tách ra là mở đường cho một lần kích
        /// Flow lọt vào giữa hai bước, nạp điểm mới với toạ độ cũ mà không có dấu hiệu gì.</para>
        /// </summary>
        virtual VisionPose TriggerCalib(EVisionProcedure procedure, const AxisSnapshot& axes, std::stop_token ct) = 0;

        /// <summary>
        /// Nút <b>"Clear Point"</b> của mọi module hiệu chuẩn (N-point Calibration, Translation Calibration) trong Flow
        /// của <paramref name="procedure"/> — xoá danh sách điểm đã nạp, gọi TRƯỚC điểm đầu tiên của một chuỗi nạp
        /// điểm. Không chạy Flow, không kích camera. Flow không có module hiệu chuẩn nào hoặc SDK báo lỗi thì trả lỗi —
        /// chuỗi nạp điểm dừng trước khi chạy trục nào.
        /// </summary>
        virtual CalibClearResult ClearCalibPoints(EVisionProcedure procedure) = 0;

        /// <summary>
        /// Đối chiếu danh mục biến toàn cục với solution đang nạp. apply = false chỉ kiểm tra; true thì tạo
        /// biến còn thiếu, ghi giá trị đầu, rồi lưu solution nếu có biến mới — không bao giờ đổi giá trị hay
        /// kiểu của biến đã có (<see cref="GlobalVarSync"/>).
        /// </summary>
        virtual GlobalVarSyncReport SyncGlobalVariables(const std::vector<GlobalVarSpec>& specs, bool apply) = 0;

        /// <summary>
        /// Đọc tên + kiểu các output một Flow đang khai ở tab <b>Output Params</b>, KHÔNG chạy Flow —
        /// nền của tool "Kiểm tra Output của các Flow". Không chạy Flow là điều kiện bắt buộc: Flow nạp điểm hiệu chuẩn
        /// mà chạy thì nạp thêm một điểm rác vào module N-point.
        /// </summary>
        virtual VmFlowOutputs ReadFlowOutputs(const std::string& flow) = 0;

        /// <summary>
        /// Gọi một Flow theo hợp đồng "App chấp hành": ghi NGHIÊM các biến toàn cục, chạy Flow, đọc output số
        /// — tất cả trong một lần giữ khoá lệnh. Lỗi và quá giờ trả về <see cref="VmCallResult::Ok"/> = false
        /// kèm lý do.
        /// </summary>
        virtual VmCallResult CallFlow(const VmCallRequest& request, std::stop_token ct) = 0;

        /// <summary>Lưu solution đang nạp. Trả CHUỖI RỖNG nếu lưu được, chuỗi lỗi nếu không.</summary>
        virtual std::string SaveSolution() = 0;

        /// <summary>Ảnh chụp được của lần gọi Flow gần nhất — trang Main hiển thị để operator nhìn được CAM đang thấy
        /// gì, không bắt buộc phải có (nullptr nếu solution chưa khai output ảnh, hoặc đang chạy mô phỏng).</summary>
        virtual std::shared_ptr<const ImageData> LastImage() const = 0;
        /// <summary>Nozzle 1…7 của ảnh <see cref="LastImage"/>; 0 = chưa có ảnh / không rõ.</summary>
        virtual int LastImageNozzle() const = 0;
    };
}
