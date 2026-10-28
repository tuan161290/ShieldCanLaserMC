#pragma once
#include "Communication/IVisionDevice.h"
#include "Data/SystemData.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>
    /// Thiết bị "thô" HIKROBOT VisionMaster 4.4 trên <b>SDK C++</b> (<c>iMVS-6000PlatformSDK</c>), chạy NGAY
    /// TRONG tiến trình của app — PLAN.md §2.1, §6. Thay cho <c>Lib.VisionMaster\VisionMasterDevice.cs</c>
    /// (SDK .NET) cộng phần <c>VisionConnect.cs</c> ghép các lời gọi mức thấp thành thao tác của
    /// <see cref="IVisionDevice"/>.
    ///
    /// <para><b>Vai trò.</b> <see cref="VisionConnect"/> sở hữu lớp này và lo: kiểm trạng thái, khoá lệnh, canh giờ
    /// <c>VisionTriggerTimeoutMs</c>, alarm, log. Lớp này vì vậy là hàm CHẶN, không tự canh giờ, không phát alarm,
    /// không có khoá lệnh riêng; lỗi SDK thì NÉM <see cref="VisionDeviceError"/> (what() = thông báo, Code = mã
    /// lỗi SDK). Gọi khi chưa nạp solution thì trả kết quả thất bại sạch, không ném.</para>
    ///
    /// <para><b>Tên Flow và tên output KHÔNG nằm trong lớp này</b> — đọc từ <see cref="SystemData::VisionOutputs"/>
    /// qua hàm truyền vào hàm tạo ở MỖI lần gọi, nên sửa ở trang Setting có hiệu lực ngay, không phải tạo lại
    /// thiết bị.</para>
    ///
    /// <para><b>Luồng.</b> Giống bản C#: không có luồng riêng cho SDK, không ràng buộc luồng — lệnh chạy trên
    /// đúng luồng nền gọi vào (bản C# là <c>Task.Run</c> dưới <c>_cmdLock</c>). Vì các hàm thêm cho UI
    /// (<see cref="RenderSource"/>, <see cref="FlowNames"/>) đi vòng qua khoá lệnh của VisionConnect, mọi lần
    /// chạm SDK ở đây được xếp hàng bằng MỘT mutex nội bộ; luồng lệnh chờ mutex đó, còn hai hàm cho UI chỉ
    /// thử khoá trong chốc lát rồi bỏ (trả nullptr / danh sách đã nhớ) — luồng UI không bao giờ đứng chờ một
    /// Flow đang chạy. Chạy Flow: <c>IVmProcedure::Run()</c> là lệnh chặn (bIsWait = true), sau đó chờ
    /// callback "Flow xong" của SDK theo kiểu sự kiện (condition variable, nghe <c>std::stop_token</c>), có
    /// chặn trên ngắn — xem VisionMasterDevice.cpp. Callback của SDK đến từ luồng của SDK và chỉ đặt cờ.</para>
    ///
    /// <para><b>Một thiết bị cho cả tiến trình.</b> Solution của SDK là singleton toàn tiến trình (như
    /// <c>VmSolution.Instance</c> bản .NET); tạo instance lần hai khi lần đầu còn sống làm SDK hỏng (đo trên
    /// máy dev 2026-10-06), nên <see cref="Connect"/> dùng lại instance đang có thay vì tạo mới.</para>
    ///
    /// <para><b>Chưa chạy được với solution thật</b>: trên máy dev <c>LoadSolution</c> trả 0xE0000000
    /// (IMVS_EC_VERSION) với mọi file. Mọi đoạn sau khi nạp viết theo header + mẫu VC của hãng, có ghi nguồn
    /// ở từng chỗ trong .cpp.</para>
    /// </summary>
    class VisionMasterDevice final : public IVisionDevice
    {
    public:
        /// <param name="sys">Nơi lấy cấu hình hiện hành (<c>[]{ return McDataManager::SysData(); }</c>). Trả
        /// nullptr hoặc để rỗng thì dùng bộ tên mặc định của <see cref="VisionOutputNames"/>.</param>
        explicit VisionMasterDevice(std::function<std::shared_ptr<SystemData>()> sys);
        ~VisionMasterDevice() override;

        VisionMasterDevice(const VisionMasterDevice&) = delete;
        VisionMasterDevice& operator=(const VisionMasterDevice&) = delete;

        /// <summary>Cho <see cref="VisionConnect::SetRealDeviceFactory"/>.</summary>
        static std::shared_ptr<IVisionDevice> Create(std::function<std::shared_ptr<SystemData>()> sys);

        bool IsConnected() const override;
        bool IsSolutionLoaded() const override;

        /// <summary>Tạo (hoặc dùng lại) instance solution của SDK, đặt mã hoá chuỗi UTF-8, đăng ký callback.
        /// Gọi lại khi đã kết nối thì trả true ngay. Lỗi thì NÉM <see cref="VisionDeviceError"/>.</summary>
        bool Connect() override;

        /// <summary>
        /// <c>Shutdown()</c> của bản C#: gỡ callback, đóng solution, huỷ instance solution. Gọi nhiều lần được.
        /// Sau đó <see cref="Connect"/> lại được trong cùng tiến trình.
        ///
        /// <para>KHÔNG gọi <c>DisposeResource()</c> của SDK: hàm đó chỉ được chạy MỘT lần trước khi thoát chương
        /// trình — sau nó <c>CreateSolutionInstance</c> trả 0xE0000006 (đo trên máy dev) — nên tách ra
        /// <see cref="DisposeSdk"/>. Các control ActiveX của VisionMaster phải được Dispose TRƯỚC khi gọi
        /// hàm này (bàn giao VM-WinUI3 §6.5).</para>
        /// </summary>
        void Disconnect() override;

        /// <summary>Nạp solution (.solw, đường dẫn UTF-8). Thành công trả true; lỗi SDK thì NÉM
        /// <see cref="VisionDeviceError"/> có mã lỗi trong thông báo. Nạp hỏng thì
        /// <see cref="IsSolutionLoaded"/> giữ nguyên giá trị cũ, như bản C#.</summary>
        bool LoadSolution(const std::string& solutionPath) override;

        VisionPose TriggerCalib(EVisionProcedure procedure, const AxisSnapshot& axes, std::stop_token ct) override;
        /// <summary>Duyệt module của Flow (<c>IVmProcedure::GetAllModuleList</c>), module nào là N-point Calibration hoặc
        /// Translation Calibration thì gọi <c>GetParamObj()->DoClearPoint()</c> — như <c>ModuParams.DoClearPoint()</c> bản
        /// .NET của ShieldCanAssy.</summary>
        CalibClearResult ClearCalibPoints(EVisionProcedure procedure) override;
        GlobalVarSyncReport SyncGlobalVariables(const std::vector<GlobalVarSpec>& specs, bool apply) override;
        VmFlowOutputs ReadFlowOutputs(const std::string& flow) override;
        VmCallResult CallFlow(const VmCallRequest& request, std::stop_token ct) override;
        std::string SaveSolution() override;

        std::shared_ptr<const ImageData> LastImage() const override;
        int LastImageNozzle() const override;

        // ── phần thêm cho UI (bản WPF bind thẳng đối tượng VM nên C# không có) ──

        /// <summary>
        /// Con trỏ đưa cho <c>VmRenderControl.SetParamsInfo</c> (DISPID 0xB) để control tự hiện kết quả:
        /// <c>"T1"</c> → cả Flow T1; <c>"T1.Module"</c> hoặc <c>"T1.Group.Module"</c> → một module của Flow đó
        /// (đúng chuỗi <see cref="VisionOutputNames::RenderPath"/> trả về).
        ///
        /// <para>Trả nullptr khi: chưa nạp solution, đường dẫn không khớp Flow/module nào, SDK báo lỗi, hoặc
        /// SDK đang bận chạy một Flow (hàm này KHÔNG chờ — gọi lại ở nhịp timer sau). Con trỏ thuộc về SDK:
        /// đưa cho control NGAY, không cất giữ; nó hết hiệu lực ở lần gọi RenderSource kế tiếp hoặc khi nạp
        /// lại / đóng solution.</para>
        /// </summary>
        void* RenderSource(const std::string& path);

        /// <summary>Tên các Flow (procedure) của solution đang nạp; rỗng nếu chưa nạp. SDK đang bận thì trả
        /// danh sách đọc được lần gần nhất.</summary>
        std::vector<std::string> FlowNames();

        /// <summary>Chuỗi build của SDK (<c>IMVS_PF_GetBuildVersion</c>); rỗng nếu máy không có SDK.</summary>
        std::string SdkVersion();

        /// <summary><c>DisposeResource()</c> của SDK — gọi ĐÚNG MỘT LẦN lúc thoát app, sau khi đã Dispose các
        /// control ActiveX và <see cref="Disconnect"/> (bàn giao VM-WinUI3 §6.5). Gọi lần hai không làm gì.
        /// Sau hàm này mọi <see cref="Connect"/> trong tiến trình đều thất bại.</summary>
        static void DisposeSdk();

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
