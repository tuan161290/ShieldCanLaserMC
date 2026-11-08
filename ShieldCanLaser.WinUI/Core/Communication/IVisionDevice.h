#pragma once
#include "Communication/ImageData.h"
#include "Communication/VmContract.h"

#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace scs
{
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
        /// Kiểm đường dẫn "Flow.Group" trong solution đang nạp, KHÔNG chạy Flow (chạy là kích camera): Flow có trong
        /// solution, module có trong Flow. Rỗng = đạt, khác rỗng = lý do. SDK C++ không liệt kê được output của một Group
        /// khi chưa chạy, nên output <c>Result</c> chỉ kiểm được ở lần chạy đầu.
        /// </summary>
        virtual std::string CheckModulePath(const std::string& path) = 0;

        /// <summary>
        /// Chạy một Flow rồi đọc output số — của chính Flow, hoặc của module <see cref="VmCallRequest::Module"/> — trong một
        /// lần giữ khoá lệnh. Lỗi và quá giờ trả về <see cref="VmCallResult::Ok"/> = false kèm lý do.
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
