#include "Utility/SingleInstanceGuard.h"

#include "Base/Text.h"

#include <mutex>
#include <string>
#include <windows.h>

#pragma comment(lib, "user32.lib")

namespace scs
{
    namespace
    {
        std::mutex g_lock;
        HANDLE g_mutex = nullptr;

        std::wstring ExeNameOfProcess(DWORD pid)
        {
            std::wstring name;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h) return name;
            wchar_t buf[MAX_PATH * 4];
            DWORD len = (DWORD)(sizeof(buf) / sizeof(buf[0]));
            if (QueryFullProcessImageNameW(h, 0, buf, &len))
            {
                name.assign(buf, len);
                size_t slash = name.find_last_of(L"\\/");
                if (slash != std::wstring::npos) name.erase(0, slash + 1);
            }
            CloseHandle(h);
            return name;
        }

        struct FindCtx { DWORD self; std::wstring exe; HWND found; };

        BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
        {
            auto* ctx = reinterpret_cast<FindCtx*>(lp);
            // Tương đương Process.MainWindowHandle: cửa sổ top-level, đang hiện, không có owner.
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == 0 || pid == ctx->self) return TRUE;
            if (_wcsicmp(ExeNameOfProcess(pid).c_str(), ctx->exe.c_str()) != 0) return TRUE;
            ctx->found = hwnd;
            return FALSE;
        }
    }

    bool SingleInstanceGuard::TryLock(std::string_view mutexName)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        HANDLE h = CreateMutexW(nullptr, TRUE, base::Widen(mutexName).c_str());
        bool createdNew = h != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
        if (!createdNew)
        {
            if (h) CloseHandle(h);
            g_mutex = nullptr;
            return false;
        }
        g_mutex = h;
        return true;
    }

    void SingleInstanceGuard::Release()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (!g_mutex) return;
        ReleaseMutex(g_mutex);   // sai luồng thì hàm trả lỗi — bỏ qua như bản C#
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }

    void SingleInstanceGuard::BringExistingToFront()
    {
        try
        {
            FindCtx ctx{ GetCurrentProcessId(), ExeNameOfProcess(GetCurrentProcessId()), nullptr };
            if (ctx.exe.empty()) return;
            EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
            if (!ctx.found) return;
            ShowWindow(ctx.found, SW_RESTORE);
            SetForegroundWindow(ctx.found);
        }
        catch (...) { }
    }
}
