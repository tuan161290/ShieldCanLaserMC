#pragma once
#include <memory>
#include <string>

#include "Base/Event.h"
#include "Communication/GlobalVarSync.h"
#include "Communication/IIoDevice.h"
#include "Communication/MotionConnect.h"
#include "Communication/VisionConnect.h"
#include "Communication/VmOutCheck.h"
#include "Controller/McFunction.h"
#include "Controller/McStatus.h"
#include "Controller/Routine_Auto.h"
#include "Controller/Routine_Calib.h"
#include "Controller/Routine_Origin.h"
#include "Controller/SafetyMonitor.h"
#include "Infor/Enums.h"

namespace scs
{
    /// <summary>
    /// Vòng điều khiển chính và máy trạng thái.
    ///
    /// <para>Một thread nền duy nhất quét thiết bị, chạy SafetyMonitor, xử lý nút bấm và cập nhật đèn
    /// tháp với chu kỳ <c>ControlTickMs</c>. Mọi lệnh từ UI chỉ ĐẶT YÊU CẦU (<see cref="RequestStart"/>…)
    /// rồi trả về ngay — vòng quét mới là nơi quyết định làm gì, nên không có đường nào để UI phát lệnh
    /// chuyển động vòng qua interlock.</para>
    ///
    /// <para>Bộ khung kế thừa ShieldCanAssy. Quy trình máy Laser Marking nằm ở <see cref="Routine_Auto"/>; START lúc chu
    /// trình chờ thay tray được chuyển cho chu trình (tray đã thay).</para>
    /// </summary>
    class McControl
    {
    public:
        McControl() = delete;

        static McStatus& Status();
        static MotionConnect& Motion();
        /// <summary>nullptr trước lần Init() đầu tiên (cả năm con trỏ dưới đây).</summary>
        static std::shared_ptr<IIoDevice> Io();
        static std::shared_ptr<VisionConnect> Vision();
        static std::shared_ptr<SafetyMonitor> Safety();
        static std::shared_ptr<McFunction> Func();
        /// <summary>Chu trình Auto.</summary>
        static std::shared_ptr<Routine_Auto> AutoRoutine();
        /// <summary>Các bước hiệu chuẩn vision của trang Vision Edit.</summary>
        static std::shared_ptr<Routine_Calib> Calib();

        /// <summary>Giây đã chạy của lượt Auto hiện tại — bộ đếm "cycle time hiện tại" ở trang Main.</summary>
        static double CurrentCycleSec();
        /// <summary>Số SP đã gắn xong của lượt Auto hiện tại — Tack Time lượt đang chạy ở trang Main.</summary>
        static int CurrentCycleProducts();

        /// <summary>Bấm AUTO mà chưa đủ điều kiện vào sẵn sàng — máy ở lại MANUAL, kèm lý do để UI bật hộp thoại.
        /// Bắn từ vòng quét (thread nền).</summary>
        static base::Event<std::string>& AutoModeRefused();

        static bool IsInitialized();

        /// <summary>Dựng thiết bị, kết nối, chạy vòng quét + watchdog. Gọi lại được sau <see cref="Shutdown"/>.
        /// CHẶN trong lúc kết nối bus — gọi từ luồng khởi động, không gọi lúc vòng quét đang chạy.</summary>
        static void Init();

        /// <summary>Huỷ mọi chuyển động, dừng và JOIN vòng quét, watchdog, routine đang chạy và việc nền
        /// (nạp solution, reset alarm trục), tắt servo (máy thật), đóng thiết bị. CHẶN.</summary>
        static void Shutdown();

        /// <summary>
        /// Đóng rồi mở lại bus EtherCAT với cấu hình HIỆN TẠI (nút Connect All Axis ở trang Setting) —
        /// không cần khởi động lại app sau khi sửa vị trí slave/định danh driver. Chỉ cho phép khi không có
        /// chu trình nào chạy: đóng bus là mọi trục mất servo. CHẶN (vài giây) — gọi từ luồng nền.
        /// </summary>
        /// <returns>chuỗi RỖNG nếu mở được bus; không thì lý do.</returns>
        static std::string ReconnectEtherCat();

        /// <summary>
        /// Tắt servo rồi đóng bus EtherCAT — cần trước khi Scan ở trang Setting (SOEM chỉ có một ngữ cảnh, không
        /// quét được trong lúc bus đang mở). Mọi trục sẽ báo mất kết nối tới khi Connect All Axis lại. CHẶN.
        /// </summary>
        /// <returns>chuỗi RỖNG nếu đã đóng; không thì lý do từ chối.</returns>
        static std::string DisconnectEtherCat();

        /// <summary>Số lần nạp solution thành công — trang Main gắn khung ảnh CAM sau lần nạp đầu, và gắn lại
        /// khi số này đổi.</summary>
        static int VisionLoadCount();

        /// <summary>Nạp lại solution theo <see cref="SystemData::VisionSolutionPath"/> HIỆN TẠI — gọi từ trang
        /// Setting sau khi đổi đường dẫn. Không chặn (nạp trên luồng nền).</summary>
        /// <returns>Lý do từ chối, chuỗi RỖNG nếu đã yêu cầu nạp.</returns>
        static std::string ReloadVisionSolution();

        /// <summary>Kiểm tra (<paramref name="apply"/> = false) hoặc tạo các biến toàn cục mà các Flow cần trong
        /// solution đang nạp — danh mục ở <see cref="VisionGlobalVarCatalog"/>. CHẶN — gọi từ luồng nền.</summary>
        static GlobalVarSyncReport SyncVisionGlobalVariables(bool apply);

        /// <summary>Đối chiếu tab Output Params của các Flow với hợp đồng — đọc khai báo, KHÔNG chạy Flow nào.
        /// CHẶN — gọi từ luồng nền.</summary>
        static VmOutCheckReport CheckVisionFlowOutputs();

        // Các hàm Request* gọi được từ bất kỳ thread nào (kể cả UI); vòng quét mới thực sự xử lý.

        /// <summary>Đặt chế độ Auto/Manual — gọi từ nút chọn mode trên trang Main. Vòng quét mới thực sự áp dụng.</summary>
        static void RequestSetMode(EMcMode mode);

        /// <summary>Nút START VẬT LÝ: lúc Armed thì khởi động chu trình, lúc tạm dừng thì chạy tiếp. KHÔNG có nút
        /// mềm nào chạy thẳng máy (quy tắc an toàn kế thừa ShieldCanAssy) — nút START trên màn hình chỉ gọi
        /// <see cref="RequestArm"/>.</summary>
        static void RequestStart();
        static void RequestStop();
        /// <summary>Dừng ở checkpoint — chạy nốt bước đang làm rồi thoát chu trình. Dùng khi chuyển MANUAL lúc đang chạy.</summary>
        static void RequestPause();
        static void RequestReset();
        static void RequestOrigin();
        /// <summary>Nút Init cạnh Về gốc trang Main — đưa máy về vị trí chờ, không về gốc lại.</summary>
        static void RequestInit();
        static void RequestMuteBuzzer();
        /// <summary>Nút START mềm trên màn hình — chỉ đưa máy vào trạng thái sẵn sàng (Armed), không chạy máy.</summary>
        static void RequestArm();

        /// <summary>Còn routine (Về gốc / Init / Auto) đang chạy. Gọi được từ mọi luồng.</summary>
        static bool RoutineBusy();

        /// <summary>Nút RESET TRAY trang Main: hai tray vừa thay — số hàng đã chạy về 0. Đang chờ thay tray thì tương đương
        /// bấm START. Trả lý do từ chối, rỗng = đã làm.</summary>
        static std::string ResetTrays();

        /// <summary>Bảng đèn tháp: dừng là VÀNG, sẵn sàng AUTO là XANH NHÁY, chạy là XANH IM, lỗi là ĐỎ.</summary>
        static ETowerLamp ComputeLamp();

    private:
        struct Impl;
    };
}
