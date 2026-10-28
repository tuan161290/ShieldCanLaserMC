#pragma once
// Bộ kiểm thử tối giản cho Core: mỗi nhóm là một hàm `int RunXxxTests()` trả về số phép kiểm hỏng,
// main.cpp gọi lần lượt. Không dùng framework ngoài để build được bằng đúng một lệnh cl.
#include <cstdio>
#include <string>

namespace testkit
{
    inline int& Failures() { static int n = 0; return n; }
    inline int& Checks()   { static int n = 0; return n; }

    inline void Report(bool ok, const char* expr, const char* file, int line, const std::string& note = {})
    {
        ++Checks();
        if (ok) return;
        ++Failures();
        std::printf("  FAIL %s:%d  %s%s%s\n", file, line, expr, note.empty() ? "" : "  -- ", note.c_str());
    }

    /// <summary>In tiêu đề nhóm và trả số lỗi phát sinh trong nhóm khi ra khỏi phạm vi.</summary>
    struct Group
    {
        const char* name;
        int failuresBefore;
        int checksBefore;
        explicit Group(const char* n) : name(n), failuresBefore(Failures()), checksBefore(Checks())
        {
            std::printf("[%s]\n", name);
        }
        int Result() const
        {
            int failed = Failures() - failuresBefore;
            std::printf("[%s] %d/%d ok\n", name, (Checks() - checksBefore) - failed, Checks() - checksBefore);
            return failed;
        }
    };
}

#define CHECK(expr)            ::testkit::Report(!!(expr), #expr, __FILE__, __LINE__)
#define CHECK_MSG(expr, note)  ::testkit::Report(!!(expr), #expr, __FILE__, __LINE__, (note))
#define CHECK_NEAR(a, b, tol)  ::testkit::Report(((a) - (b)) <= (tol) && ((b) - (a)) <= (tol), #a " ~= " #b, __FILE__, __LINE__)
