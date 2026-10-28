#include "EtherCat/Soem/RtCpu.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <format>
#include <tuple>

namespace ecm::rt
{
    std::vector<LogicalCpu> EnumerateCpus()
    {
        std::vector<LogicalCpu> cpus;

        ULONG length = 0;
        GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
        if (length == 0) return cpus;

        std::vector<uint8_t> buffer(length);
        if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                                        length, &length, GetCurrentProcess(), 0))
            return cpus;

        // Các phần tử có kích thước ghi trong chính nó (Size) — không được bước bằng sizeof.
        for (ULONG offset = 0; offset + sizeof(DWORD) * 2 <= length;)
        {
            auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data() + offset);
            if (info->Size == 0) break;
            if (info->Type == CpuSetInformation)
            {
                LogicalCpu c;
                c.CpuSetId = info->CpuSet.Id;
                c.Group = info->CpuSet.Group;
                c.LogicalIndex = info->CpuSet.LogicalProcessorIndex;
                c.CoreIndex = info->CpuSet.CoreIndex;
                c.EfficiencyClass = info->CpuSet.EfficiencyClass;
                c.Parked = info->CpuSet.Parked != 0;
                c.Allocated = info->CpuSet.Allocated != 0;
                c.AllocatedToThisProcess = info->CpuSet.AllocatedToTargetProcess != 0;
                cpus.push_back(c);
            }
            offset += info->Size;
        }
        return cpus;
    }

    CoreReservation ReserveCore(int requestedLogicalIndex)
    {
        CoreReservation r;
        auto cpus = EnumerateCpus();
        if (cpus.empty())
        {
            r.Description = "Cannot read CPU list (GetSystemCpuSetInformation) — cyclic thread not pinned";
            return r;
        }

        const LogicalCpu* chosen = nullptr;

        if (requestedLogicalIndex >= 0)
        {
            for (const auto& c : cpus)
                if (c.Group == 0 && c.LogicalIndex == requestedLogicalIndex) { chosen = &c; break; }

            if (chosen == nullptr)
            {
                r.Description = std::format("CPU {} does not exist ({} logical CPUs) — cyclic thread not pinned",
                                            requestedLogicalIndex, cpus.size());
                return r;
            }
            if (cpus.size() < 2)
            {
                r.Description = "Only 1 logical CPU — cannot reserve core";
                return r;
            }
        }
        else
        {
            if (cpus.size() < 4)
            {
                r.Description = std::format("Only {} logical CPUs — core not auto-reserved (set RtCpuIndex to force)",
                                            cpus.size());
                return r;
            }

            // Core chứa CPU 0 của group 0: nơi Windows dồn phần lớn ngắt và DPC.
            int core0 = -1;
            for (const auto& c : cpus)
                if (c.Group == 0 && c.LogicalIndex == 0) { core0 = c.CoreIndex; break; }

            for (const auto& c : cpus)
            {
                if (c.Group == 0 && c.CoreIndex == core0) continue;
                if (c.Allocated && !c.AllocatedToThisProcess) continue;   // tiến trình khác đã giữ riêng

                if (chosen == nullptr) { chosen = &c; continue; }
                // Ưu tiên: lớp hiệu năng THẤP hơn (E-core trên CPU lai), rồi tới group/chỉ số cao hơn.
                // Việc của một chu kỳ rất nhẹ; đo trên i7-12700H, E-core quay chờ liên tục cho jitter
                // ngang P-core (vài µs) mà toả nhiệt ít hơn hẳn và để nguyên P-core cho xử lý ảnh.
                // CPU không lai thì mọi core cùng lớp, luật này thành "core số cao nhất".
                auto key = [](const LogicalCpu& x) {
                    return std::tuple<int, int, int>(-(int)x.EfficiencyClass, x.Group, x.LogicalIndex);
                };
                if (key(c) > key(*chosen)) chosen = &c;
            }

            if (chosen == nullptr)
            {
                r.Description = "No free core to reserve — cyclic thread not pinned";
                return r;
            }
        }

        r.LogicalIndex = chosen->LogicalIndex;
        r.Group = chosen->Group;
        r.CpuSetId = chosen->CpuSetId;

        std::vector<ULONG> others;
        for (const auto& c : cpus)
        {
            if (c.Group == chosen->Group && c.CoreIndex == chosen->CoreIndex)
                r.CoreCpuSetIds.push_back(c.CpuSetId);
            else
                others.push_back(c.CpuSetId);
        }

        if (others.empty())
        {
            r.Description = std::format("CPU {}: no other CPU left for rest of app — not reserved", r.LogicalIndex);
            return r;
        }

        r.Reserved = SetProcessDefaultCpuSets(GetCurrentProcess(), others.data(), (ULONG)others.size()) != FALSE;
        r.Description = r.Reserved
            ? std::format("Reserved CPU {} (physical core {}, {} logical CPUs, efficiency class {}) — {} CPUs left for rest of app",
                          r.LogicalIndex, (int)chosen->CoreIndex, r.CoreCpuSetIds.size(), (int)chosen->EfficiencyClass, others.size())
            : std::format("SetProcessDefaultCpuSets error {} — CPU {} pinned only, NOT reserved",
                          GetLastError(), r.LogicalIndex);
        return r;
    }

    void ReleaseCore(const CoreReservation& reservation)
    {
        if (reservation.Reserved)
            SetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0);
    }

    bool PinCurrentThread(const CoreReservation& reservation)
    {
        if (reservation.LogicalIndex < 0) return false;

        // CPU set riêng của luồng thắng CPU set mặc định của tiến trình (vốn đã loại core này ra).
        ULONG id = reservation.CpuSetId;
        bool selected = SetThreadSelectedCpuSets(GetCurrentThread(), &id, 1) != FALSE;

        // CPU set chỉ là ưu tiên "mềm": đo trên máy lai P/E-core cho thấy khi core được chọn đang
        // "parked" (hệ thống rảnh), bộ lập lịch đưa luồng sang CPU khác ở hơn 60% số chu kỳ. Affinity
        // cứng buộc luồng ở đúng CPU đó. Mask của luồng vẫn nằm trong mask của tiến trình vì ta không
        // đụng tới affinity của tiến trình (chỉ đổi CPU set mặc định).
        GROUP_AFFINITY affinity{};
        affinity.Group = reservation.Group;
        affinity.Mask = (KAFFINITY)1 << reservation.LogicalIndex;
        bool hard = SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != FALSE;

        return selected && hard;
    }

    void DisableProcessThrottling()
    {
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.StateMask = 0;   // 0 = tắt throttling cho các bit nêu trong ControlMask
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)))
        {
            // Windows cũ hơn 11 không biết bit IGNORE_TIMER_RESOLUTION và từ chối cả lời gọi.
            state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
        }
    }

    int CurrentCpuIndex()
    {
        PROCESSOR_NUMBER n{};
        GetCurrentProcessorNumberEx(&n);
        return n.Number;
    }
}
