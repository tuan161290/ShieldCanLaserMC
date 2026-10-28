#include "EtherCat/Soem/RtCyclicThread.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>

#include <format>

#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace ecm::rt
{
    const uint32_t LatenessBucketLimitUs[LatenessBuckets - 1] = { 50, 100, 250, 500, 1000, 2000 };

    namespace
    {
        long long QpcNow()
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return t.QuadPart;
        }

        long long QpcFrequency()
        {
            static const long long frequency = [] {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                return f.QuadPart;
            }();
            return frequency;
        }

        double TicksToUs(long long ticks)
        {
            return (double)ticks * 1000000.0 / (double)QpcFrequency();
        }
    }

    RtCyclicThread::~RtCyclicThread()
    {
        Stop();
    }

    bool RtCyclicThread::Start(const RtOptions& options, CycleFn cycle, void* user, int expectedWkc, std::string* info)
    {
        Stop();
        if (cycle == nullptr) return false;

        m_options = options;
        if (m_options.PeriodUs < 250) m_options.PeriodUs = 250;   // dưới mức này Npcap không còn theo kịp
        m_cycle = cycle;
        m_user = user;
        m_expectedWkc = expectedWkc;
        m_info.clear();

        DisableProcessThrottling();

        // Dành riêng core TRƯỚC khi tạo luồng: lúc luồng chu kỳ bắt đầu chạy thì phần còn lại của
        // tiến trình đã rời core đó.
        m_reservation = m_options.ReserveCore ? ReserveCore(m_options.CpuIndex) : CoreReservation{};

        m_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_stop.store(false, std::memory_order_release);
        m_resetStats.store(true, std::memory_order_release);
        m_thread = std::thread([this] { ThreadMain(); });

        if (m_readyEvent != nullptr)
        {
            WaitForSingleObject(static_cast<HANDLE>(m_readyEvent), 5000);
            CloseHandle(static_cast<HANDLE>(m_readyEvent));
            m_readyEvent = nullptr;
        }
        if (info != nullptr) *info = m_info;
        return true;
    }

    void RtCyclicThread::Stop()
    {
        m_stop.store(true, std::memory_order_release);
        if (m_thread.joinable()) m_thread.join();
        m_running.store(false, std::memory_order_release);

        ReleaseCore(m_reservation);
        m_reservation = CoreReservation{};
    }

    void RtCyclicThread::ResetStats()
    {
        m_resetStats.store(true, std::memory_order_release);
    }

    RtStats RtCyclicThread::Stats() const
    {
        RtStats copy;
        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            uint32_t s1 = m_statsSeq.load(std::memory_order_acquire);
            if (s1 & 1u) { YieldProcessor(); continue; }
            copy = m_stats;
            if (m_statsSeq.load(std::memory_order_acquire) == s1) break;
        }
        return copy;
    }

    double RtCyclicThread::LastCycleAgeUs() const
    {
        long long last = m_lastCycleQpc.load(std::memory_order_acquire);
        if (last == 0) return 0.0;
        return TicksToUs(QpcNow() - last);
    }

    void RtCyclicThread::ThreadMain()
    {
        SetThreadDescription(GetCurrentThread(), L"EtherCAT cyclic (RT)");

        bool pinned = m_options.ReserveCore && PinCurrentThread(m_reservation);

        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        int priority = GetThreadPriority(GetCurrentThread());

        // Luồng này không bao giờ được coi là "nền" để hạ xung nhịp.
        THREAD_POWER_THROTTLING_STATE throttling{};
        throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        throttling.StateMask = 0;
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof(throttling));

        // Timer độ phân giải cao (Windows 10 1803+) không phụ thuộc timeBeginPeriod. Máy cũ hơn:
        // rơi về timer thường + nâng độ phân giải toàn hệ thống lên 1 ms như bản cũ.
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        bool highRes = timer != nullptr;
        if (!highRes)
        {
            timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
            timeBeginPeriod(1);
        }

        const long long frequency = QpcFrequency();
        const long long periodTicks = (long long)m_options.PeriodUs * frequency / 1000000;
        const long long spinTicks = (long long)m_options.SpinUs * frequency / 1000000;
        const bool neverSleep = m_options.SpinUs >= m_options.PeriodUs || timer == nullptr;

        m_info = std::format(
            "EtherCAT cyclic thread: period {} µs, spin {} µs{}, priority {}{}, high-res timer: {}; {}",
            m_options.PeriodUs, m_options.SpinUs, neverSleep ? " (no sleep, core at 100%)" : "",
            priority, priority == THREAD_PRIORITY_TIME_CRITICAL ? " (TIME_CRITICAL)" : " (TIME_CRITICAL NOT reached)",
            highRes ? "yes" : "no",
            m_options.ReserveCore
                ? (pinned ? m_reservation.Description : m_reservation.Description + " [not pinned]")
                : std::string("core not reserved (ReserveCore = false)"));

        m_running.store(true, std::memory_order_release);
        if (m_readyEvent != nullptr) SetEvent(static_cast<HANDLE>(m_readyEvent));

        long long next = QpcNow() + periodTicks;
        long long lastStart = 0;

        while (!m_stop.load(std::memory_order_acquire))
        {
            // ── Pha 1: ngủ tới trước hạn một đoạn SpinUs ──
            long long now = QpcNow();
            if (!neverSleep)
            {
                long long sleepTicks = next - now - spinTicks;
                long long sleep100ns = sleepTicks * 10000000 / frequency;
                if (sleep100ns > 0)
                {
                    LARGE_INTEGER due;
                    due.QuadPart = -sleep100ns;   // âm = tương đối
                    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
                        WaitForSingleObject(timer, INFINITE);
                }
            }

            // ── Pha 2: quay chờ tới đúng hạn ──
            now = QpcNow();
            while (now < next)
            {
                YieldProcessor();
                now = QpcNow();
            }

            const long long start = now;
            const long long lateness = start - next;

            // ── Việc của chu kỳ: phát + nhận khung PDO ──
            int wkc = m_cycle(m_user);

            const long long end = QpcNow();
            m_lastCycleQpc.store(start, std::memory_order_release);

            // ── Thống kê (seqlock, không bao giờ chờ bên đọc) ──
            const bool reset = m_resetStats.exchange(false, std::memory_order_acq_rel);
            m_statsSeq.fetch_add(1, std::memory_order_release);
            if (reset)
            {
                m_stats = RtStats{};
                m_jitterSumUs = m_latenessSumUs = m_scanSumUs = 0;
                m_jitterCount = 0;
                lastStart = 0;
            }

            RtStats& s = m_stats;
            ++s.Cycles;

            if (lastStart != 0)
            {
                double jitterUs = TicksToUs(start - lastStart) - (double)m_options.PeriodUs;
                if (m_jitterCount == 0) s.PeriodJitterMinUs = s.PeriodJitterMaxUs = jitterUs;
                else
                {
                    if (jitterUs < s.PeriodJitterMinUs) s.PeriodJitterMinUs = jitterUs;
                    if (jitterUs > s.PeriodJitterMaxUs) s.PeriodJitterMaxUs = jitterUs;
                }
                m_jitterSumUs += jitterUs;
                ++m_jitterCount;
                s.PeriodJitterAvgUs = m_jitterSumUs / (double)m_jitterCount;
            }
            lastStart = start;

            double latenessUs = TicksToUs(lateness);
            if (latenessUs > s.LatenessMaxUs) s.LatenessMaxUs = latenessUs;
            m_latenessSumUs += latenessUs;
            s.LatenessAvgUs = m_latenessSumUs / (double)s.Cycles;
            int bucket = LatenessBuckets - 1;
            for (int i = 0; i < LatenessBuckets - 1; ++i)
                if (latenessUs < (double)LatenessBucketLimitUs[i]) { bucket = i; break; }
            ++s.LatenessHistogram[bucket];

            double scanUs = TicksToUs(end - start);
            if (s.Cycles == 1) s.ScanTimeMinUs = s.ScanTimeMaxUs = scanUs;
            else
            {
                if (scanUs < s.ScanTimeMinUs) s.ScanTimeMinUs = scanUs;
                if (scanUs > s.ScanTimeMaxUs) s.ScanTimeMaxUs = scanUs;
            }
            m_scanSumUs += scanUs;
            s.ScanTimeAvgUs = m_scanSumUs / (double)s.Cycles;

            s.LastWkc = wkc;
            s.ExpectedWkc = m_expectedWkc;
            if (m_expectedWkc > 0 && wkc != m_expectedWkc) ++s.WkcErrors;

            int cpu = CurrentCpuIndex();
            if (pinned && cpu != m_reservation.LogicalIndex) ++s.CpuMigrations;
            s.CpuIndex = cpu;
            s.CoreReserved = m_reservation.Reserved;
            s.Pinned = pinned;
            s.HighResTimer = highRes;
            s.ThreadPriority = priority;
            s.PeriodUs = m_options.PeriodUs;
            s.SpinUs = m_options.SpinUs;

            // ── Hạn kế tiếp. Đã lỡ hạn (ngủ quá giấc, hoặc khung về quá chậm) thì BỎ các nhịp đã lỡ
            // thay vì phát dồn: phát dồn chỉ làm nhịp kế tiếp méo thêm, còn driver chỉ cần khung đều. ──
            next += periodTicks;
            while (next <= end)
            {
                next += periodTicks;
                ++s.Overruns;
            }

            m_statsSeq.fetch_add(1, std::memory_order_release);
        }

        if (timer != nullptr) CloseHandle(timer);
        if (!highRes) timeEndPeriod(1);
        m_running.store(false, std::memory_order_release);
    }
}
