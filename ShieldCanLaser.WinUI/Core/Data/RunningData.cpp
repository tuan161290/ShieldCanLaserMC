#include "Data/RunningData.h"

#include "Utility/JsonHelper.h"

namespace scs
{
    using jsonx::json;
    using jsonx::Get;

    void RunningData::RecordCycle(double sec, int products)
    {
        if (sec <= 0.01 || products <= 0) return;
        LastCycleSec = sec;
        LastProductTimeSec = sec / products;
        std::vector<double> list(RecentProductTimes);
        for (int i = 0; i < products; i++) list.push_back(LastProductTimeSec);
        if ((int)list.size() > CycleAvgWindow) list.erase(list.begin(), list.end() - CycleAvgWindow);
        double sum = 0;
        for (double t : list) sum += t;
        AvgProductTimeSec = sum / (double)list.size();
        RecentProductTimes = std::move(list);
    }

    void RunningData::ResetCounters()
    {
        CountOk = CountNg = CountTotal = ConsecutiveNg = 0;
        LastProductTimeSec = AvgProductTimeSec = LastCycleSec = 0;
        RecentProductTimes.clear();
    }

    void RunningData::ResetTrays()
    {
        TrayRowsDone[0] = TrayRowsDone[1] = 0;
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
        j["TrayRowsDone"] = json::array({ TrayRowsDone[0], TrayRowsDone[1] });
        j["LastRowResult"] = LastRowResult;
        j["LastRowTray"] = LastRowTray;
        j["LastRowIndex"] = LastRowIndex;
        j["CountOk"] = CountOk;
        j["CountNg"] = CountNg;
        j["CountTotal"] = CountTotal;
        j["ConsecutiveNg"] = ConsecutiveNg;
        j["LastProductTimeSec"] = LastProductTimeSec;
        j["RecentProductTimes"] = RecentProductTimes;
        j["AvgProductTimeSec"] = AvgProductTimeSec;
        j["LastCycleSec"] = LastCycleSec;
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
        if (auto t = j.find("TrayRowsDone"); t != j.end() && t->is_array())
        {
            for (size_t i = 0; i < 2 && i < t->size(); ++i)
                if ((*t)[i].is_number_integer()) r->TrayRowsDone[i] = (*t)[i].get<int>();
        }
        Get(j, "LastRowResult", r->LastRowResult);
        Get(j, "LastRowTray", r->LastRowTray);
        Get(j, "LastRowIndex", r->LastRowIndex);
        Get(j, "CountOk", r->CountOk);
        Get(j, "CountNg", r->CountNg);
        Get(j, "CountTotal", r->CountTotal);
        Get(j, "ConsecutiveNg", r->ConsecutiveNg);
        Get(j, "LastProductTimeSec", r->LastProductTimeSec);
        Get(j, "RecentProductTimes", r->RecentProductTimes);
        Get(j, "AvgProductTimeSec", r->AvgProductTimeSec);
        Get(j, "LastCycleSec", r->LastCycleSec);
        return r;
    }
}
