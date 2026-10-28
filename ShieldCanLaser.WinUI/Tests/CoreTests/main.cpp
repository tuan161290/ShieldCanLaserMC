// CoreTests.exe                         chạy mọi nhóm kiểm thử (mã thoát 0 = đạt hết)
// CoreTests.exe rtbench [giây] [tải] [cpu]  đo jitter luồng chu kỳ trên máy này (xem RtBench.cpp)
// CoreTests.exe vmprobe                  thử thiết bị VisionMaster thật (SDK C++) — cần máy đã cài VisionMaster 4.4
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "TestKit.h"

int RunInfraTests();
int RunEtherCatSimTests();
int RunRtTests();
int RunMotionTests();
int RunVisionTests();
int RunControllerTests();
int RunRtBench(int seconds, int loadThreads, int cpuIndex);
int RunVisionMasterProbe();

namespace
{
    int RunGroup(const char* name, int (*fn)())
    {
        try { return fn(); }
        catch (const std::exception& ex) { std::printf("[%s] EXCEPTION: %s\n", name, ex.what()); return 1; }
        catch (...) { std::printf("[%s] EXCEPTION (unknown)\n", name); return 1; }
    }
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    if (argc > 1 && (std::strcmp(argv[1], "rtbench") == 0 || std::strcmp(argv[1], "--rtbench") == 0))
    {
        int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        int load = argc > 3 ? std::atoi(argv[3]) : (int)si.dwNumberOfProcessors;
        int cpu = argc > 4 ? std::atoi(argv[4]) : -1;
        return RunRtBench(seconds, load, cpu);
    }

    if (argc > 1 && std::strcmp(argv[1], "vmprobe") == 0)
    {
        int failedChecks = RunVisionMasterProbe();
        std::printf("\nvmprobe: %d kiem thu hong\n", failedChecks);
        return failedChecks == 0 ? 0 : 1;
    }

    int failed = 0;
    failed += RunGroup("Infra", RunInfraTests);
    failed += RunGroup("EtherCatSim", RunEtherCatSimTests);
    failed += RunGroup("Realtime", RunRtTests);
    failed += RunGroup("Motion", RunMotionTests);
    failed += RunGroup("Vision", RunVisionTests);
    failed += RunGroup("Controller", RunControllerTests);

    std::printf("\n%d kiem thu, %d hong\n", testkit::Checks(), testkit::Failures());
    return (failed == 0 && testkit::Failures() == 0) ? 0 : 1;
}
