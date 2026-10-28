// Đo jitter của luồng chu kỳ trên CHÍNH máy này, không cần phần cứng EtherCAT: so timer multimedia
// (cách của bản C++/CLI cũ) với luồng thời gian thực ghim core, ở vài mức quay chờ, có và không có
// tải CPU do chính tiến trình tạo ra (giả lập UI + VisionMaster đang bận).
//
//   CoreTests.exe --rtbench [giây mỗi cấu hình = 5] [số luồng tải = số CPU logic]
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <timeapi.h>

#include "EtherCat/Soem/RtCyclicThread.h"

#pragma comment(lib, "winmm.lib")

namespace
{
    constexpr unsigned PeriodMs = 4;

    double QpcUs()
    {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
    }

    // Việc giả của một chu kỳ: ~150 µs bận, cỡ một lần phát + nhận khung qua Npcap.
    int FakeCycle(void*)
    {
        double until = QpcUs() + 150.0;
        while (QpcUs() < until) YieldProcessor();
        return 1;
    }

    struct Summary
    {
        const char* name;
        unsigned long long cycles;
        double jitterMin, jitterMax, jitterAvgAbs;
        unsigned long long over500, over2000;
    };

    void Print(const Summary& s)
    {
        std::printf("  %-44s %7llu chu ky | jitter min %+8.1f  max %+8.1f  |tb| %6.1f us | >0.5ms: %llu  >2ms: %llu\n",
                    s.name, s.cycles, s.jitterMin, s.jitterMax, s.jitterAvgAbs, s.over500, s.over2000);
    }

    // ── Cách cũ: timer multimedia, callback trên luồng của winmm ──
    struct MmState
    {
        double last = 0;
        unsigned long long n = 0, over500 = 0, over2000 = 0;
        double min = 0, max = 0, sumAbs = 0;
    };

    void CALLBACK MmCallback(UINT, UINT, DWORD_PTR user, DWORD_PTR, DWORD_PTR)
    {
        auto* st = reinterpret_cast<MmState*>(user);
        double now = QpcUs();
        if (st->last != 0)
        {
            double j = (now - st->last) - PeriodMs * 1000.0;
            if (st->n == 0) st->min = st->max = j;
            if (j < st->min) st->min = j;
            if (j > st->max) st->max = j;
            st->sumAbs += std::fabs(j);
            if (std::fabs(j) > 500) ++st->over500;
            if (std::fabs(j) > 2000) ++st->over2000;
            ++st->n;
        }
        st->last = now;
        FakeCycle(nullptr);
    }

    Summary RunMultimediaTimer(int seconds)
    {
        MmState st;
        timeBeginPeriod(1);
        MMRESULT id = timeSetEvent(PeriodMs, 1, MmCallback, reinterpret_cast<DWORD_PTR>(&st), TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
        Sleep(seconds * 1000);
        timeKillEvent(id);
        timeEndPeriod(1);
        return { "timer multimedia (ban cu, khong ghim core)", st.n, st.min, st.max, st.n ? st.sumAbs / st.n : 0, st.over500, st.over2000 };
    }

    int g_cpuIndex = -1;

    void RunRtThread(const char* name, int seconds, uint32_t spinUs, bool reserve)
    {
        ecm::rt::RtCyclicThread rt;
        ecm::rt::RtOptions opt;
        opt.PeriodUs = PeriodMs * 1000;
        opt.SpinUs = spinUs;
        opt.ReserveCore = reserve;
        opt.CpuIndex = g_cpuIndex;
        std::string info;
        rt.Start(opt, FakeCycle, nullptr, 1, &info);
        Sleep(seconds * 1000);
        ecm::rt::RtStats s = rt.Stats();
        rt.Stop();

        // Biểu đồ độ trễ: các khoảng từ 500 µs trở lên.
        unsigned long long over500 = s.LatenessHistogram[4] + s.LatenessHistogram[5] + s.LatenessHistogram[6];
        unsigned long long over2000 = s.LatenessHistogram[6];
        Print({ name, s.Cycles, s.PeriodJitterMinUs, s.PeriodJitterMaxUs, s.LatenessAvgUs, over500, over2000 });
        std::printf("      cpu=%d ghim=%d danh_rieng=%d doi_cpu=%llu lo_nhip=%llu | tre so voi han: max %.1f tb %.2f us\n",
                    s.CpuIndex, s.Pinned ? 1 : 0, s.CoreReserved ? 1 : 0, s.CpuMigrations, s.Overruns, s.LatenessMaxUs, s.LatenessAvgUs);
    }

    void RunAll(int seconds)
    {
        Print(RunMultimediaTimer(seconds));
        RunRtThread("luong RT, KHONG ghim core, quay cho 1000us", seconds, 1000, false);
        RunRtThread("luong RT ghim core, chi timer (quay cho 0)", seconds, 0, true);
        RunRtThread("luong RT ghim core, quay cho 1000us", seconds, 1000, true);
        RunRtThread("luong RT ghim core, khong ngu (100% core)", seconds, PeriodMs * 1000, true);
    }
}

int RunRtBench(int seconds, int loadThreads, int cpuIndex)
{
    g_cpuIndex = cpuIndex;
    auto cpus = ecm::rt::EnumerateCpus();
    std::printf("May co %zu CPU logic:", cpus.size());
    for (const auto& c : cpus) std::printf(" %d(core %d, lop %d)", c.LogicalIndex, c.CoreIndex, c.EfficiencyClass);
    std::printf("\nChu ky %u ms, moi cau hinh %d giay.\n", PeriodMs, seconds);

    std::printf("\n== Khong tai ==\n");
    RunAll(seconds);

    if (loadThreads > 0)
    {
        std::printf("\n== Co tai: %d luong ban cua chinh tien trinh (gia lap UI + vision dang ban) ==\n", loadThreads);
        std::atomic<bool> stop{ false };
        std::vector<std::thread> load;
        for (int i = 0; i < loadThreads; ++i)
            load.emplace_back([&stop] {
                volatile double x = 1.0;
                while (!stop.load(std::memory_order_relaxed))
                    for (int k = 0; k < 10000; ++k) x = x * 1.0000001 + 0.1;
            });
        RunAll(seconds);
        stop = true;
        for (auto& t : load) t.join();
    }
    return 0;
}
