#include "pch.h"
#include "Ui/AppHost.h"

#include <shellapi.h>

#include "Base/Async.h"
#include "Base/Text.h"
#include "Communication/VisionConnect.h"
#include "Controller/McControl.h"
#include "Data/McDataManager.h"
#include "Data/UserAccess.h"
#include "Infor/AlarmCatalog.h"
#include "Infor/AlarmCode.h"
#include "Infor/AlarmManager.h"
#include "SysConfig/AppPaths.h"
#include "Ui/UiUtil.h"
#include "Utility/Log/LogWorker.h"
#include "Utility/SingleInstanceGuard.h"
#include "Utility/UiThread.h"
#include "Vision/VisionMasterDevice.h"

namespace ui
{
    namespace
    {
        // Cùng tên mutex với bản WPF: hai bản không chạy song song được (chung dữ liệu, chung phần cứng).
        constexpr char SingleInstanceMutexName[] =
            "Global\\ShieldCanLaser_SingleInstance_{9F3C2B41-6A7D-4E58-9C10-2B5E7A1D8C33}";

        AppOptions ParseCommandLine()
        {
            AppOptions options;
            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argv == nullptr) return options;
            for (int i = 1; i < argc; ++i)
            {
                std::wstring arg = argv[i];
                if (arg == L"--root" && i + 1 < argc) options.DataRoot = base::Narrow(argv[++i]);
                else if (arg == L"--shot" && i + 1 < argc) options.ScreenshotDir = base::Narrow(argv[++i]);
            }
            LocalFree(argv);
            return options;
        }

        AppOptions& MutableOptions()
        {
            static AppOptions options = ParseCommandLine();
            return options;
        }

        bool g_started = false;
        bool g_shutdown = false;
        bool g_ownsInstanceLock = false;
    }

    const AppOptions& AppHost::Options() { return MutableOptions(); }

    bool AppHost::Startup()
    {
        using scs::LogLevel;
        using scs::LogWorker;
        const AppOptions& options = Options();

        // Core chuyển việc về luồng UI qua đây (không phụ thuộc WinUI).
        scs::UiThread::SetDispatcher([](std::function<void()> fn) { PostUi(std::move(fn)); });

        // Chạy với gốc dữ liệu riêng (chụp màn hình, chạy thử) thì không tranh chấp với bản đang chạy máy.
        if (options.DataRoot.empty())
        {
            if (!scs::SingleInstanceGuard::TryLock(SingleInstanceMutexName))
            {
                scs::SingleInstanceGuard::BringExistingToFront();
                MessageBoxW(nullptr, L"Application is already running.", L"Shield CAN Laser Marking MC", MB_OK | MB_ICONEXCLAMATION);
                return false;
            }
            g_ownsInstanceLock = true;
        }

        scs::AppPaths::Init(options.DataRoot);
        LogWorker::Instance().Init(scs::AppPaths::Root());
        LogWorker::Instance().RecordLog("===== Application started =====");
        if (scs::AppPaths::UsingFallbackRoot())
            LogWorker::Instance().RecordLog(
                std::string("Cannot use ") + scs::AppPaths::DefaultRoot + " — machine data stored at " +
                    base::PathToU8(scs::AppPaths::Root()),
                LogLevel::Warning);

        base::SetBackgroundErrorHandler([](const char* what) {
            LogWorker::Instance().RecordLog(std::string("Background thread error: ") + what, LogLevel::Error);
        });

        scs::AlarmCatalog::Load();
        scs::McDataManager::LoadSystemData();
        scs::McDataManager::LoadRunningData();
        scs::McDataManager::LoadRecentModel();
        scs::UserAccess::Load();   // Config/users.json — mở app luôn ở cấp Operator

        // Thiết bị VisionMaster thật (SDK C++, chạy trong tiến trình này) — dùng cả khi chạy mô phỏng, trừ khi Setting
        // chọn "Vision giả" (SystemData::VisionSimulation). Đăng ký TRƯỚC McControl::Init, nơi VisionConnect được dựng.
        // Chế độ chụp ảnh không đăng ký: ảnh chụp phải giống nhau ở mọi máy, có cài VisionMaster hay không.
        if (options.ScreenshotDir.empty())
            scs::VisionConnect::SetRealDeviceFactory([] {
                return scs::VisionMasterDevice::Create([] { return scs::McDataManager::SysData(); });
            });

        try
        {
            scs::McControl::Init();
        }
        catch (const std::exception& ex)
        {
            // Lỗi driver/thiết bị không được chặn cả shell không mở được — thanh trạng thái đã hiển thị rõ
            // thiết bị nào chưa kết nối.
            LogWorker::Instance().RecordLog(std::string("McControl::Init failed: ") + ex.what(), LogLevel::Error);
            scs::AlarmManager::Raise(scs::AlarmCode::ConfigInvalid, "System", ex.what());
        }

        g_started = true;
        return true;
    }

    void AppHost::Shutdown()
    {
        if (g_shutdown) return;
        g_shutdown = true;

        if (g_started)
        {
            try
            {
                scs::LogWorker::Instance().RecordLog("===== Application exited =====");
                scs::McControl::Shutdown();
                // DisposeResource của SDK VisionMaster: đúng một lần, sau khi các control ActiveX (OnClosing của
                // các trang) và thiết bị (McControl::Shutdown) đã giải phóng — bàn giao VM-WinUI3 §6.5.
                scs::VisionMasterDevice::DisposeSdk();
                scs::McDataManager::SaveRunningData();
                scs::AlarmManager::FlushCsv();
            }
            catch (...) {}

            scs::LogWorker::Instance().Shutdown();
        }
        if (g_ownsInstanceLock) scs::SingleInstanceGuard::Release();
    }

    void AppHost::OnUnhandledUiException(const std::string& message)
    {
        scs::LogWorker::Instance().RecordLog("Unhandled exception: " + message, scs::LogLevel::Error);
        // Phát dạng SỰ KIỆN: Raise trơn thì không nơi nào gỡ nguyên nhân, RESET không bao giờ xoá được.
        scs::AlarmManager::RaiseEvent(scs::AlarmCode::RoutineCrashed, "UI", message);
    }
}
