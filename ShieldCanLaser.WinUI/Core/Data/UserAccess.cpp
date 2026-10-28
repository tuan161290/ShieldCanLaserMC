#include "Data/UserAccess.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>

#include "Base/Text.h"
#include "SysConfig/AppPaths.h"
#include "Utility/JsonHelper.h"
#include "Utility/Log/LogWorker.h"

#pragma comment(lib, "bcrypt.lib")

namespace scs
{
    using jsonx::json;

    const char* ToString(EUserLevel level)
    {
        return level == EUserLevel::Engineer ? "ENGINEER" : "OPERATOR";
    }

    namespace
    {
        struct State
        {
            std::mutex lock;
            EUserLevel level{ EUserLevel::Operator };
            std::string salt;          // hex
            std::string hash;          // hex SHA-256(salt + mật khẩu); trống = dùng mật khẩu mặc định
            int idleMinutes{ 10 };
            UserAccess::Clock::time_point lastActivity{ UserAccess::Clock::now() };
            base::Event<EUserLevel> changed;
        };

        State& S()
        {
            static State s;
            return s;
        }

        void Log(const std::string& msg, LogLevel level = LogLevel::Info)
        {
            LogWorker::Instance().RecordLog(msg, level, "User");
        }

        std::string Hex(const unsigned char* data, size_t len)
        {
            static const char* digits = "0123456789abcdef";
            std::string s;
            s.reserve(len * 2);
            for (size_t i = 0; i < len; ++i)
            {
                s += digits[data[i] >> 4];
                s += digits[data[i] & 15];
            }
            return s;
        }

        std::string Sha256Hex(const std::string& text)
        {
            unsigned char digest[32]{};
            BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)text.data(), (ULONG)text.size(), digest, sizeof(digest));
            return Hex(digest, sizeof(digest));
        }

        std::string NewSalt()
        {
            unsigned char bytes[16]{};
            BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            return Hex(bytes, sizeof(bytes));
        }

        /// Mật khẩu master (Tuấn 2026-10-21) — luôn đăng nhập được, kể cả khi quên mật khẩu Engineer; dùng được làm "mật
        /// khẩu hiện tại" khi đổi mật khẩu. Chỉ giữ băm SHA-256(MasterSalt + mật khẩu), không giữ chữ rõ.
        constexpr const char* MasterSalt = "scs-master-salt";
        constexpr const char* MasterHash = "e970a5bd44f28c2f5802bd4f32b60459c644bd100b3638f0cb1a6710e9532249";

        bool IsMaster(const std::string& password)
        {
            return Sha256Hex(MasterSalt + password) == MasterHash;
        }

        bool Matches(const State& s, const std::string& password)
        {
            if (IsMaster(password)) return true;
            if (s.hash.empty()) return password == UserAccess::DefaultPassword;
            return Sha256Hex(s.salt + password) == s.hash;
        }

        bool Save(const State& s, std::string& why)
        {
            json j;
            j["EngineerSalt"] = s.salt;
            j["EngineerHash"] = s.hash;
            j["IdleLogoutMinutes"] = s.idleMinutes;
            try
            {
                std::filesystem::create_directories(AppPaths::UsersFile().parent_path());
                std::filesystem::path tmp = AppPaths::UsersFile();
                tmp += ".tmp";
                if (!base::WriteAllText(tmp, j.dump(2))) { why = "cannot write users.json"; return false; }
                std::filesystem::rename(tmp, AppPaths::UsersFile());
                return true;
            }
            catch (const std::exception& ex)
            {
                why = std::string("users.json save failed: ") + ex.what();
                return false;
            }
        }

        /// Đổi cấp dưới khoá, phát sự kiện ngoài khoá (handler có thể đọc lại Level()).
        void SetLevel(EUserLevel level)
        {
            State& s = S();
            {
                std::lock_guard<std::mutex> g(s.lock);
                if (s.level == level) return;
                s.level = level;
                s.lastActivity = UserAccess::Clock::now();
            }
            s.changed.Raise(level);
        }
    }

    void UserAccess::Load()
    {
        State& s = S();
        std::string text;
        std::string salt, hash;
        int idle = 10;
        bool found = base::ReadAllText(AppPaths::UsersFile(), text);
        if (found)
        {
            json j;
            if (jsonx::ParseObject(text, j))
            {
                jsonx::Get(j, "EngineerSalt", salt);
                jsonx::Get(j, "EngineerHash", hash);
                jsonx::Get(j, "IdleLogoutMinutes", idle);
            }
            else Log("users.json unreadable — Engineer password reset to default", LogLevel::Warning);
        }
        {
            std::lock_guard<std::mutex> g(s.lock);
            s.salt = salt;
            s.hash = salt.empty() ? std::string() : hash;
            s.idleMinutes = std::clamp(idle, 1, 240);
        }
        SetLevel(EUserLevel::Operator);
        if (UsingDefaultPassword()) Log("Engineer password is the default — change it after login", LogLevel::Warning);
    }

    EUserLevel UserAccess::Level()
    {
        std::lock_guard<std::mutex> g(S().lock);
        return S().level;
    }

    bool UserAccess::UsingDefaultPassword()
    {
        std::lock_guard<std::mutex> g(S().lock);
        return S().hash.empty();
    }

    int UserAccess::IdleLogoutMinutes()
    {
        std::lock_guard<std::mutex> g(S().lock);
        return S().idleMinutes;
    }

    bool UserAccess::Login(const std::string& password, std::string& why)
    {
        bool ok;
        {
            std::lock_guard<std::mutex> g(S().lock);
            ok = Matches(S(), password);
        }
        if (!ok)
        {
            why = "Wrong password";
            Log("Engineer login failed (wrong password)", LogLevel::Warning);
            return false;
        }
        SetLevel(EUserLevel::Engineer);
        Log(IsMaster(password) ? "Engineer logged in with MASTER password" : "Engineer logged in",
            IsMaster(password) ? LogLevel::Warning : LogLevel::Info);
        return true;
    }

    void UserAccess::Logout(const std::string& reason)
    {
        if (Level() == EUserLevel::Operator) return;
        SetLevel(EUserLevel::Operator);
        Log("Engineer logged out (" + reason + ")");
    }

    bool UserAccess::ChangePassword(const std::string& oldPassword, const std::string& newPassword, std::string& why)
    {
        State& s = S();
        std::lock_guard<std::mutex> g(s.lock);
        if (s.level != EUserLevel::Engineer) { why = "Login as Engineer first"; return false; }
        if (!Matches(s, oldPassword)) { why = "Current password is wrong"; return false; }
        if ((int)newPassword.size() < MinPasswordLength) { why = std::format("New password needs at least {} characters", MinPasswordLength); return false; }
        State next{};
        next.salt = NewSalt();
        next.hash = Sha256Hex(next.salt + newPassword);
        next.idleMinutes = s.idleMinutes;
        if (!Save(next, why)) return false;
        s.salt = next.salt;
        s.hash = next.hash;
        Log(IsMaster(oldPassword) ? "Engineer password reset with MASTER password" : "Engineer password changed",
            IsMaster(oldPassword) ? LogLevel::Warning : LogLevel::Info);
        return true;
    }

    void UserAccess::Touch(Clock::time_point now)
    {
        std::lock_guard<std::mutex> g(S().lock);
        S().lastActivity = now;
    }

    bool UserAccess::CheckIdle(Clock::time_point now)
    {
        int minutes;
        {
            std::lock_guard<std::mutex> g(S().lock);
            State& s = S();
            if (s.level == EUserLevel::Operator || now - s.lastActivity < std::chrono::minutes(s.idleMinutes)) return false;
            minutes = s.idleMinutes;
        }
        Logout(std::format("no activity for {} min", minutes));
        return true;
    }

    int UserAccess::SecondsUntilIdleLogout(Clock::time_point now)
    {
        std::lock_guard<std::mutex> g(S().lock);
        State& s = S();
        if (s.level == EUserLevel::Operator) return 0;
        auto left = std::chrono::minutes(s.idleMinutes) - (now - s.lastActivity);
        return (int)std::max<long long>(0, std::chrono::duration_cast<std::chrono::seconds>(left).count());
    }

    base::Event<EUserLevel>& UserAccess::LevelChanged() { return S().changed; }
}
