#include "pch.h"

#include <exception>

#include "Base/Async.h"
#include "Utility/Log/LogWorker.h"

#pragma warning(push)
#pragma warning(disable : 4819)
#include "VMException.h"
#pragma warning(pop)

// C++/WinRT đổi exception thành HRESULT ở MỌI ranh giới ABI qua winrt::to_hresult(), vốn noexcept và chỉ
// hiểu hresult_error / std::exception. Bất kỳ thứ gì khác (vd. VisionMasterSDK::CVmException lọt ra từ một
// handler) sẽ kết thúc tiến trình trong im lặng — và tiến trình này đang giữ bus EtherCAT. Dồn tất cả qua
// handler này: ghi log rồi trả E_FAIL, app sống tiếp.
namespace
{
    void Log(std::string const& text)
    {
        try { scs::LogWorker::Instance().RecordLog(text, scs::LogLevel::Error, "Crash"); }
        catch (...) {}
        OutputDebugStringA((text + "\n").c_str());
    }

    int32_t __stdcall ToHresultHandler(void* returnAddress) noexcept
    {
        try { throw; }
        catch (winrt::hresult_error const& e)
        {
            return e.code();
        }
        catch (VisionMasterSDK::CVmException const& e)
        {
            Log(std::format("CVmException 0x{:X} reached WinRT boundary (caller {})", (unsigned)e.GetErrorCode(), returnAddress));
            return E_FAIL;
        }
        catch (base::OperationCanceled const&)
        {
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        catch (std::exception const& e)
        {
            Log(std::format("std::exception reached WinRT boundary: {} (caller {})", e.what(), returnAddress));
            return E_FAIL;
        }
        catch (...)
        {
            Log(std::format("Unknown exception reached WinRT boundary (caller {})", returnAddress));
            return E_FAIL;
        }
    }

    struct Installer
    {
        Installer()
        {
            ::winrt_to_hresult_handler = ToHresultHandler;
            std::set_terminate([] {
                Log("std::terminate called — process terminating");
                try { scs::LogWorker::Instance().Flush(); } catch (...) {}
                abort();
            });
        }
    } g_installer;
}
