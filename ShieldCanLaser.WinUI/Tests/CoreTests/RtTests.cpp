// Kiểm thử lõi thời gian thực không cần phần cứng: vùng đệm process-image (không rách dữ liệu khi
// luồng app và luồng chu kỳ chạy đồng thời) và luồng chu kỳ (ghim core, dành riêng core, dừng sạch).
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "EtherCat/Soem/ProcessImage.h"
#include "EtherCat/Soem/RtCyclicThread.h"
#include "TestKit.h"

namespace
{
    bool AllEqual(const uint8_t* p, size_t n)
    {
        for (size_t i = 1; i < n; ++i)
            if (p[i] != p[0]) return false;
        return true;
    }

    void TestProcessImageBasics()
    {
        uint8_t out[32]{}, in[32]{};
        out[4] = 0xAB;   // trạng thái ngõ ra có sẵn lúc gắn
        ecm::rt::ProcessImage image;
        image.Attach(out, sizeof(out), in, sizeof(in));

        uint8_t shadow = 0;
        image.ReadOutputShadow(4, &shadow, 1);
        CHECK(shadow == 0xAB);

        image.Output<uint16_t>(3, 0x1234);
        CHECK(out[3] == 0 && out[4] == 0xAB);        // chưa tới IOmap cho đến khi luồng chu kỳ rút hàng đợi
        uint16_t staged = 0;
        image.ReadOutputShadow(3, &staged, 2);
        CHECK(staged == 0x1234);                     // nhưng phía app đã thấy giá trị mình vừa đặt

        image.ApplyPendingOutputs();
        CHECK(out[3] == 0x34 && out[4] == 0x12);

        // Ghi ngoài phạm vi và ghi quá 8 byte bị bỏ qua, không làm hỏng vùng nhớ.
        uint8_t big[16]{ 1 };
        image.WriteOutput(30, big, 4);
        image.WriteOutput(0, big, 16);
        image.ApplyPendingOutputs();
        CHECK(out[30] == 0 && out[31] == 0 && out[0] == 0);

        in[10] = 0x78; in[11] = 0x56; in[12] = 0x34; in[13] = 0x12;
        CHECK(image.Input<uint32_t>(10) == 0);       // bản chụp cũ
        image.PublishInputs();
        CHECK(image.Input<uint32_t>(10) == 0x12345678u);
        CHECK(image.Input<uint32_t>(30) == 0);       // đọc ngoài phạm vi trả 0

        image.Detach();
        image.Output<uint16_t>(3, 0xFFFF);           // đã tách: không được đụng vào out[]
        image.ApplyPendingOutputs();
        CHECK(out[3] == 0x34 && out[4] == 0x12);
    }

    // Hai luồng app ghi liên tục các trường 8 byte LỆCH HÀNG (offset 3 và 13) với mọi byte bằng nhau;
    // luồng "chu kỳ" rút hàng đợi và chép ngõ vào. Bất kỳ lần nào thấy một trường có byte khác nhau là
    // một lần dữ liệu bị rách.
    void TestProcessImageConcurrency()
    {
        uint8_t out[32]{}, in[32]{};
        ecm::rt::ProcessImage image;
        image.Attach(out, sizeof(out), in, sizeof(in));

        std::atomic<bool> stop{ false };
        std::atomic<long long> tornOut{ 0 }, tornIn{ 0 }, cycles{ 0 }, reads{ 0 };

        std::thread cyclic([&] {
            uint8_t v = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                image.ApplyPendingOutputs();
                if (!AllEqual(out + 3, 8) || !AllEqual(out + 13, 8)) ++tornOut;
                std::memset(in + 5, ++v, 16);
                image.PublishInputs();
                ++cycles;
            }
        });

        auto writer = [&](size_t offset) {
            uint8_t v = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                uint8_t field[8];
                std::memset(field, ++v, sizeof(field));
                image.WriteOutput(offset, field, sizeof(field));
            }
        };
        std::thread w1(writer, (size_t)3), w2(writer, (size_t)13);

        auto reader = [&] {
            while (!stop.load(std::memory_order_relaxed))
            {
                uint8_t field[8];
                image.ReadInput(7, field, sizeof(field));
                if (!AllEqual(field, sizeof(field))) ++tornIn;
                ++reads;
            }
        };
        std::thread r1(reader), r2(reader);

        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        stop = true;
        cyclic.join(); w1.join(); w2.join(); r1.join(); r2.join();

        CHECK(cycles.load() > 1000);
        CHECK(reads.load() > 1000);
        CHECK_MSG(tornOut.load() == 0, "ngo ra bi rach: " + std::to_string(tornOut.load()));
        CHECK_MSG(tornIn.load() == 0, "ngo vao bi rach: " + std::to_string(tornIn.load()));
    }

    std::atomic<int> g_cycleCalls{ 0 };
    int CountingCycle(void*) { ++g_cycleCalls; return 3; }

    unsigned long DefaultCpuSetCount()
    {
        ULONG ids[256]{};
        ULONG count = 0;
        GetProcessDefaultCpuSets(GetCurrentProcess(), ids, 256, &count);
        return count;
    }

    void TestCyclicThread()
    {
        auto cpus = ecm::rt::EnumerateCpus();
        CHECK(!cpus.empty());
        CHECK(DefaultCpuSetCount() == 0);   // chưa ai giới hạn tiến trình

        ecm::rt::RtCyclicThread rt;
        ecm::rt::RtOptions opt;
        opt.PeriodUs = 1000;
        opt.SpinUs = 1000;                  // không ngủ
        std::string info;
        g_cycleCalls = 0;
        CHECK(rt.Start(opt, CountingCycle, nullptr, 3, &info));
        CHECK(rt.Running());
        CHECK(!info.empty());
        std::printf("  %s\n", info.c_str());

        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        ecm::rt::RtStats s = rt.Stats();

        CHECK_MSG(s.Cycles > 350 && s.Cycles < 450, "cycles=" + std::to_string(s.Cycles));
        CHECK(s.PeriodUs == 1000);
        CHECK(s.LastWkc == 3 && s.ExpectedWkc == 3 && s.WkcErrors == 0);
        CHECK(s.ThreadPriority == THREAD_PRIORITY_TIME_CRITICAL);
        CHECK(rt.LastCycleAgeUs() < 50000.0);

        if (cpus.size() >= 4)
        {
            CHECK(s.CoreReserved);
            CHECK(s.Pinned);
            CHECK_MSG(s.CpuMigrations == 0, "doi cpu " + std::to_string(s.CpuMigrations) + " lan");
            // Phần còn lại của tiến trình đã bị đẩy khỏi core đó.
            unsigned long allowed = DefaultCpuSetCount();
            CHECK_MSG(allowed > 0 && allowed < cpus.size(), "allowed=" + std::to_string(allowed));
            // Trên core dành riêng, không ngủ: trễ so với hạn phải nhỏ hơn hẳn một chu kỳ.
            CHECK_MSG(s.LatenessMaxUs < 500.0, "lateness max " + std::to_string(s.LatenessMaxUs) + " us");
            CHECK(s.Overruns == 0);
        }
        std::printf("  cpu=%d cycles=%llu jitter[%.1f..%.1f] lateness max %.1f us, scan avg %.2f us\n",
                    s.CpuIndex, s.Cycles, s.PeriodJitterMinUs, s.PeriodJitterMaxUs, s.LatenessMaxUs, s.ScanTimeAvgUs);

        // Đếm lỗi working counter.
        rt.Stop();
        CHECK(!rt.Running());
        int callsAtStop = g_cycleCalls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(g_cycleCalls.load() == callsAtStop);          // dừng là dừng hẳn
        CHECK(DefaultCpuSetCount() == 0);                    // core được trả lại cho tiến trình

        CHECK(rt.Start(opt, CountingCycle, nullptr, 5, nullptr));   // kỳ vọng 5, callback trả 3
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        s = rt.Stats();
        CHECK(s.WkcErrors > 0 && s.WkcErrors == s.Cycles);
        rt.Stop();

        // Không dành riêng core: vẫn chạy, không đụng CPU set của tiến trình.
        opt.ReserveCore = false;
        opt.SpinUs = 200;
        CHECK(rt.Start(opt, CountingCycle, nullptr, 3, nullptr));
        CHECK(DefaultCpuSetCount() == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        s = rt.Stats();
        CHECK(!s.CoreReserved && !s.Pinned && s.Cycles > 50);
        rt.Stop();
    }
}

int RunRtTests()
{
    testkit::Group g("Realtime");
    TestProcessImageBasics();
    TestProcessImageConcurrency();
    TestCyclicThread();
    return g.Result();
}
