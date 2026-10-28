#pragma once
// Một control ActiveX của VisionMaster gắn vào một ô giữ chỗ trong XAML.
//
// WinUI 3 vẽ toàn bộ XAML đè lên mọi cửa sổ con, nên control VM không nhúng được kiểu child HWND: nó nằm
// trong một cửa sổ popup SỞ HỮU (AxHost::ConvertToPopup) và app tự cho popup bám theo ô giữ chỗ — xem
// BAN-GIAO-VM-WinUI3.md mục 3 và 6. VmPane gói trọn công thức đó và ba trường hợp tài liệu bàn giao nêu
// là "probe chưa làm":
//   * ô giữ chỗ đổi vị trí mà không đổi kích thước  → nghe LayoutUpdated, chỉ di chuyển popup khi khung đổi;
//   * chuyển trang                                   → trang gọi SetVisible(false/true) trong OnShown;
//   * lớp phủ modal mở                               → trang gọi SetEnabled(false/true) trong OnModalChanged.
//
// GỌI LỒNG XAML (dump 06/10, app sập 0xC000027B tại CXcpDispatcher::OnReentrancyProtectedWindowMessage): control VM là
// .NET và gọi sang tiến trình vServerApp; trong lúc chờ, COM bơm thông điệp. Nếu lúc đó đang ở trong một sự kiện XAML
// (Loaded, LayoutUpdated, SizeChanged, Tick của DispatcherTimer…) thì XAML nhận lại thông điệp của chính nó khi đang xử
// lý dở và tự kết thúc tiến trình. Vì vậy: Sync/SetVisible/SetEnabled chỉ GHI NHẬN rồi xếp hàng (ui::PostUi) — việc chạm
// cửa sổ VM chạy ở lượt sau, ngoài mọi sự kiện XAML; Create và các lời gọi Host() phải do trang gọi từ một việc đã xếp
// hàng như vậy (xem QueueVmWork của MainPage/VisionEditPage), không gọi thẳng trong handler XAML.
#include <memory>
#include <string>

#include "Ui/AxHost.h"

namespace ui
{
    /// <summary>CLSID + hai DISPID mọi control VM đều có. Nguồn: ComControls\Includes\VS2017\CVm*Interface.h.</summary>
    struct VmControlKind
    {
        const wchar_t* Clsid;
        DISPID GetObjectPointer;   // gọi ngay sau khi tạo để control tự khởi tạo
        DISPID Dispose;
    };

    namespace vmkind
    {
        extern const VmControlKind Render;      // VmRenderControl — ảnh + hình vẽ kết quả
        extern const VmControlKind GlobalTool;  // VmGlobalToolControl — biến toàn cục, camera, trigger…
        extern const VmControlKind Procedure;   // VmProcedureConfigControl — trình soạn flow
        extern const VmControlKind MainView;    // VmMainViewConfigControl — giao diện soạn thảo đầy đủ
    }

    // DISPID riêng của từng control (ngoài hai cái trong VmControlKind).
    namespace vmdisp
    {
        // VmRenderControl
        constexpr DISPID RenderSetImageSourceData = 0x1;
        constexpr DISPID RenderSetParamsInfo = 0xB;
        constexpr DISPID RenderSetBackground = 0x11;   // BSTR: mã màu "#RRGGBB" hoặc đường dẫn ảnh nền nhỏ (<100×100)
        constexpr DISPID RenderClearDisplayView = 0x12;
        constexpr DISPID RenderSetToolbarVisible = 0x1A;
        // VmGlobalToolControl
        constexpr DISPID GlobalOpenVariable = 0x3;
        constexpr DISPID GlobalOpenScript = 0x4;
        constexpr DISPID GlobalOpenCamera = 0x5;
        constexpr DISPID GlobalOpenTrigger = 0x6;
        constexpr DISPID GlobalOpenCommunication = 0x7;
        // VmProcedureConfigControl / VmMainViewConfigControl (cùng bộ DISPID)
        constexpr DISPID ProcBindSingleProcedure = 0x2;
        constexpr DISPID ProcBindMultiProcedure = 0x3;
        constexpr DISPID ProcLockWorkArea = 0x4;
        constexpr DISPID ProcUnlockWorkArea = 0x5;
        constexpr DISPID ProcSetParamTabEditable = 0x6;
    }

    /// <summary>HWND của cửa sổ chính — chủ của mọi popup VM. MainWindow đặt khi nội dung đã Loaded.</summary>
    void SetMainHwnd(HWND hwnd);
    HWND MainHwnd();

    class VmPane
    {
    public:
        VmPane() = default;
        ~VmPane();
        VmPane(const VmPane&) = delete;
        VmPane& operator=(const VmPane&) = delete;

        /// <summary>
        /// Tạo control và gắn vào ô giữ chỗ. Gọi trên luồng UI, từ một việc đã xếp hàng (ui::PostUi) — KHÔNG
        /// trong handler XAML, xem đầu file — sau khi cửa sổ có HWND và ô giữ chỗ đã ở trong cây hiển thị. Máy chưa đăng ký ActiveX của VM (chưa cài VisionMaster/chưa chạy
        /// ComRegister.bat) thì trả false kèm lý do — trang hiện lý do đó trong ô giữ chỗ, app chạy tiếp.
        /// </summary>
        bool Create(winrt::Microsoft::UI::Xaml::FrameworkElement const& placeholder, const VmControlKind& kind,
                    std::string* error = nullptr);

        bool Created() const { return m_created; }

        /// <summary>Đặt popup trùng khít ô giữ chỗ — xếp hàng, chạy ở lượt sau (gộp các lần gọi dồn dập). Tự được gọi
        /// khi bố cục đổi; trang gọi thêm trong OnWindowMoved.</summary>
        void Sync();

        /// <summary>Trang chứa control được hiện/ẩn (áp dụng ở lượt sau, như Sync).</summary>
        void SetVisible(bool visible);

        /// <summary>false trong lúc có lớp phủ modal: control vẫn hiện nhưng không nhận chuột/phím (áp dụng ở lượt
        /// sau).</summary>
        void SetEnabled(bool enabled);

        /// <summary>Gọi Dispose của control rồi huỷ cửa sổ. Gọi được nhiều lần.</summary>
        void Dispose();

        AxHost& Host() { return m_host; }

    private:
        /// <summary>Phần Sync chạm cửa sổ VM — chỉ gọi ngoài sự kiện XAML.</summary>
        void SyncNow();
        void RoundPopupCorners();

        AxHost m_host;
        VmControlKind m_kind{};
        bool m_created{ false };
        bool m_visible{ true };
        RECT m_lastRect{ -1, -1, -1, -1 };
        bool m_lastShown{ false };
        winrt::Microsoft::UI::Xaml::FrameworkElement m_placeholder{ nullptr };
        winrt::event_token m_layoutToken{};
        winrt::event_token m_sizeToken{};
        bool m_syncQueued{ false };
        bool m_enableQueued{ false };
        bool m_wantEnabled{ true };
        /// <summary>Việc đã xếp hàng giữ weak_ptr tới đây: VmPane bị huỷ trước khi việc chạy thì việc bỏ qua.</summary>
        std::shared_ptr<char> m_life{ std::make_shared<char>() };
    };
}
