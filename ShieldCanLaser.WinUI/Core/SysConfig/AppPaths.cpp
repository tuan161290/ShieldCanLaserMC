#include "SysConfig/AppPaths.h"

#include "Base/Text.h"

#include <mutex>
#include <windows.h>

namespace scs
{
    namespace fs = std::filesystem;

    namespace
    {
        struct State
        {
            std::mutex mutex;
            fs::path root;
            fs::path exeDirOverride;
            bool usingFallback{ false };
        };

        State& S()
        {
            static State s;
            return s;
        }

        bool CanUse(const fs::path& root)
        {
            try
            {
                fs::path drive = root.root_path();
                std::error_code ec;
                if (drive.empty() || !fs::is_directory(drive, ec)) return false;
                fs::create_directories(root);
                return true;
            }
            catch (...) { return false; }
        }
    }

    fs::path AppPaths::Root()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.root;
    }

    bool AppPaths::UsingFallbackRoot()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.usingFallback;
    }

    fs::path AppPaths::ExeDir()
    {
        {
            auto& s = S();
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.exeDirOverride.empty()) return s.exeDirOverride;
        }

        std::wstring buf(1024, L'\0');
        while (true)
        {
            DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
            if (n == 0) return {};
            if (n < buf.size()) { buf.resize(n); break; }
            buf.resize(buf.size() * 2);
        }
        return fs::path(buf).parent_path();
    }

    void AppPaths::SetExeDirForTests(const fs::path& dir)
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.exeDirOverride = dir;
    }

    void AppPaths::Init(std::string_view overrideRoot)
    {
        fs::path root;
        bool fallback = false;

        if (!base::IsBlank(overrideRoot))
        {
            root = base::U8Path(overrideRoot);
        }
        else if (CanUse(base::U8Path(DefaultRoot)))
        {
            root = base::U8Path(DefaultRoot);
        }
        else
        {
            fs::path exe = ExeDir();
            std::error_code ec;
            if (exe.empty()) exe = fs::current_path(ec);
            root = exe / "_RunData";
            fallback = true;
        }

        {
            auto& s = S();
            std::lock_guard<std::mutex> lock(s.mutex);
            s.root = root;
            s.usingFallback = fallback;
        }

        for (const auto& dir : { ConfigDir(), ModelDir(), VisionDir(), RuntimeDir(), DataDir(), ImageNgDir(), LogDir() })
        {
            std::error_code ec;
            fs::create_directories(dir, ec);
        }
    }
}
