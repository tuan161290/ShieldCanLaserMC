#include "Data/RunningData.h"

#include <algorithm>
#include <chrono>

#include "Utility/JsonHelper.h"

namespace scs
{
    using jsonx::json;
    using jsonx::Get;

    int64_t RunningData::NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void RunningData::StartCycleClock(int64_t nowMs)
    {
        if (CycleStartedAtMs != 0) return;
        CycleStartedAtMs = nowMs;
        CycleOk = 0;
        CycleHoldLast = false;
    }

    void RunningData::FinishCycle(int64_t nowMs)
    {
        if (CycleStartedAtMs == 0) return;
        LastCycleSec = std::max(0.0, (double)(nowMs - CycleStartedAtMs) / 1000.0);
        LastCycleProducts = CycleOk;
        CycleStartedAtMs = 0;
        CycleHoldLast = true;
    }

    void RunningData::ResetCounters()
    {
        CountOk = CountNg = CountTotal = ConsecutiveNg = 0;
        LastCycleSec = 0;
        LastCycleProducts = 0;
        CycleHoldLast = false;
    }

    void RunningData::ResetTrays()
    {
        for (auto& tray : Trays) tray.ClearDone();
        CycleStartedAtMs = 0;
        CycleOk = 0;
        LastRowResult.clear();
        LastRowTray = LastRowIndex = 0;
    }

    void RunningData::ResetCycleProgress()
    {
        AutoState = EAutoState::S00_Ready;
        UnsafePause = false;
    }

    std::string RunningData::ToJsonText() const
    {
        json j = json::object();
        if (!RecentModelName.empty()) j["RecentModelName"] = RecentModelName;   // null → không ghi
        j["SavedAt"] = ToIsoString(SavedAt);
        j["AutoState"] = (int)AutoState;
        j["UnsafePause"] = UnsafePause;
        j["TrayDoneCells"] = json::array({ Trays[0].Done(), Trays[1].Done() });
        j["LastRowResult"] = LastRowResult;
        j["LastRowTray"] = LastRowTray;
        j["LastRowIndex"] = LastRowIndex;
        j["CountOk"] = CountOk;
        j["CountNg"] = CountNg;
        j["CountTotal"] = CountTotal;
        j["ConsecutiveNg"] = ConsecutiveNg;
        j["LastCycleSec"] = LastCycleSec;
        j["LastCycleProducts"] = LastCycleProducts;
        j["CycleHoldLast"] = CycleHoldLast;
        j["NgRatePercent"] = NgRatePercent();   // property chỉ-đọc: ghi để xem, khi đọc thì bỏ qua
        return jsonx::Dump(j);
    }

    std::shared_ptr<RunningData> RunningData::FromJsonText(const std::string& text)
    {
        json j;
        if (!jsonx::ParseObject(text, j)) return nullptr;

        auto r = std::make_shared<RunningData>();
        Get(j, "RecentModelName", r->RecentModelName);
        jsonx::GetTime(j, "SavedAt", r->SavedAt);
        int state = (int)r->AutoState;
        Get(j, "AutoState", state);
        r->AutoState = (EAutoState)state;
        Get(j, "UnsafePause", r->UnsafePause);
        if (auto t = j.find("TrayDoneCells"); t != j.end() && t->is_array())
        {
            for (size_t i = 0; i < r->Trays.size() && i < t->size(); ++i)
            {
                std::vector<int> cells;
                for (const auto& v : (*t)[i])
                    if (v.is_number_integer()) cells.push_back(v.get<int>());
                r->Trays[i].LoadDone(std::move(cells));
            }
        }
        Get(j, "LastRowResult", r->LastRowResult);
        Get(j, "LastRowTray", r->LastRowTray);
        Get(j, "LastRowIndex", r->LastRowIndex);
        Get(j, "CountOk", r->CountOk);
        Get(j, "CountNg", r->CountNg);
        Get(j, "CountTotal", r->CountTotal);
        Get(j, "ConsecutiveNg", r->ConsecutiveNg);
        Get(j, "LastCycleSec", r->LastCycleSec);
        Get(j, "LastCycleProducts", r->LastCycleProducts);
        Get(j, "CycleHoldLast", r->CycleHoldLast);
        return r;
    }
}
