#include "Infor/AlarmManager.h"

#include "Base/Async.h"
#include "Base/Text.h"
#include "Infor/AlarmCatalog.h"
#include "SysConfig/AppPaths.h"
#include "Utility/Log/LogWorker.h"

#include <atomic>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <share.h>

namespace scs
{
    namespace fs = std::filesystem;

    namespace
    {
        using AlarmPtr = std::shared_ptr<AlarmData>;

        /// <summary>Cửa sổ gom cụm để xác định alarm gốc — PLAN.md §7.3.</summary>
        constexpr auto RootCauseWindow = std::chrono::milliseconds(200);

        struct State
        {
            std::mutex mutex;
            std::vector<AlarmPtr> active;     // theo thứ tự phát
            std::deque<AlarmPtr> history;     // dùng chung đối tượng với active (ClearedAt hiện cả ở lịch sử)
            TimePoint lastRaise{};            // TimePoint{} = DateTime.MinValue
            int maxHistory{ 200 };
            std::atomic<uint64_t> version{ 0 };
            base::Event<AlarmData> alarmRaised;

            std::mutex csvMutex;              // csvQueue
            std::deque<AlarmData> csvQueue;
            std::atomic<int> csvDraining{ 0 };
            std::mutex csvFileMutex;
        };

        State& S()
        {
            static State s;
            return s;
        }

        AlarmPtr FindActive(State& s, int code)
        {
            for (auto& a : s.active) if (a->Code == code) return a;
            return nullptr;
        }

        void EraseActive(State& s, int code)
        {
            std::erase_if(s.active, [code](const AlarmPtr& a) { return a->Code == code; });
        }

        std::string SourceFromCode(int code)
        {
            if (code >= 1100 && code < 1200) return "Safety";
            if (code >= AlarmCode::AxisCodeMin && code < AlarmCode::AxisCodeEnd)
            {
                std::string axis; AxisFault f;
                return AlarmCode::TryParseAxis(code, axis, f) ? axis : "Motion";
            }
            if (code >= 3000 && code < 3100) return "IO";
            if (code >= 3100 && code < 3200) return "EtherCAT";
            if (code >= 4000 && code < 4100) return "Vision";
            if (code >= 5000 && code < 5100) return "Auto";
            if (code >= 6000 && code < 6100) return "Data";
            return "System";
        }

        void WriteCsvLine(const AlarmData& alarm)
        {
            fs::path dir = AppPaths::LogDir() / FormatLocal(std::chrono::system_clock::now(), "yyyyMMdd");
            fs::create_directories(dir);
            fs::path file = dir / "alarm.csv";

            std::lock_guard<std::mutex> lock(S().csvFileMutex);
            std::error_code ec;
            bool isNewFile = !fs::exists(file, ec);
            bool isEmpty = isNewFile || fs::file_size(file, ec) == 0;

            FILE* f = _wfsopen(file.c_str(), L"ab", _SH_DENYWR);
            if (!f) throw std::runtime_error("cannot open alarm.csv");
            std::string text;
            if (isEmpty) text += "\xEF\xBB\xBF";
            if (isNewFile) text += std::string(AlarmData::CsvHeader) + "\r\n";
            text += alarm.ToCsvLine() + "\r\n";
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
        }

        bool PopCsv(AlarmData& a)
        {
            auto& s = S();
            std::lock_guard<std::mutex> lock(s.csvMutex);
            if (s.csvQueue.empty()) return false;
            a = std::move(s.csvQueue.front());
            s.csvQueue.pop_front();
            return true;
        }

        void DrainCsv()
        {
            auto& s = S();
            try
            {
                AlarmData a;
                while (PopCsv(a)) WriteCsvLine(a);
            }
            catch (...)
            {
                // Không được để lỗi ghi CSV làm hỏng đường phát alarm — alarm vẫn phải lên UI.
            }
            s.csvDraining.store(0);
            // Dòng được xếp hàng đúng lúc vòng lặp vừa thoát sẽ nằm lại nếu không kiểm lại.
            bool pending;
            {
                std::lock_guard<std::mutex> lock(s.csvMutex);
                pending = !s.csvQueue.empty();
            }
            if (pending && s.csvDraining.exchange(1) == 0) base::RunDetached(DrainCsv);
        }

        /// <summary>
        /// Xếp hàng ghi alarm ra CSV, ghi thật trên thread nền. Ghi thẳng là sai: Raise được gọi từ
        /// vòng quét an toàn chu kỳ 20 ms, và câu lệnh chạy NGAY SAU nó thường là lệnh dừng máy — một
        /// lần đĩa khựng vài chục ms sẽ dời đúng cái lệnh dừng đó. Hàng đợi giữ nguyên thứ tự dòng vì
        /// chỉ có một thread rút hàng tại một thời điểm.
        /// </summary>
        void AppendCsv(const AlarmData& alarm)
        {
            auto& s = S();
            {
                std::lock_guard<std::mutex> lock(s.csvMutex);
                s.csvQueue.push_back(alarm);
            }
            if (s.csvDraining.exchange(1) == 0) base::RunDetached(DrainCsv);
        }

        template <class Pred>
        bool AnyActive(Pred pred)
        {
            auto& s = S();
            std::lock_guard<std::mutex> lock(s.mutex);
            for (auto& a : s.active) if (pred(*a)) return true;
            return false;
        }
    }

    std::vector<AlarmData> AlarmManager::ActiveAlarms()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::vector<AlarmData> r;
        for (auto& a : s.active) r.push_back(*a);
        return r;
    }

    std::vector<AlarmData> AlarmManager::History()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::vector<AlarmData> r;
        for (auto& a : s.history) r.push_back(*a);
        return r;
    }

    uint64_t AlarmManager::Version() { return S().version.load(); }

    int AlarmManager::MaxHistory()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.maxHistory;
    }

    void AlarmManager::SetMaxHistory(int n)
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.maxHistory = n;
    }

    base::Event<AlarmData>& AlarmManager::AlarmRaised() { return S().alarmRaised; }

    bool AlarmManager::HasFatal()   { return AnyActive([](const AlarmData& a) { return a.Level == EAlarmLevel::Fatal; }); }
    bool AlarmManager::HasError()   { return AnyActive([](const AlarmData& a) { return a.Level >= EAlarmLevel::Error; }); }
    bool AlarmManager::HasWarning() { return AnyActive([](const AlarmData& a) { return a.Level == EAlarmLevel::Warning; }); }

    int AlarmManager::ActiveCount()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        return (int)s.active.size();
    }

    std::optional<AlarmData> AlarmManager::RootCause()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        // OrderByDescending(IsRootCause).ThenByDescending(Level).ThenBy(RaisedAt) — hoà thì lấy cái phát trước.
        const AlarmData* best = nullptr;
        for (auto& p : s.active)
        {
            const AlarmData& a = *p;
            if (best == nullptr) { best = &a; continue; }
            if (a.IsRootCause != best->IsRootCause) { if (a.IsRootCause) best = &a; continue; }
            if (a.Level != best->Level) { if (a.Level > best->Level) best = &a; continue; }
            if (a.RaisedAt < best->RaisedAt) best = &a;
        }
        return best ? std::optional<AlarmData>(*best) : std::nullopt;
    }

    AlarmData AlarmManager::Raise(int code, std::string_view source, std::string_view detail)
    {
        auto& s = S();

        // Tra bảng NGOÀI khoá của AlarmManager (AlarmCatalog có khoá riêng); kết quả chỉ dùng khi mã chưa active.
        AlarmDefinition def = AlarmCatalog::Get(code);
        AlarmData copy;

        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (AlarmPtr existing = FindActive(s, code))
            {
                bool changed = !existing->ConditionActive;
                existing->ConditionActive = true;
                if (!detail.empty() && existing->Detail != detail) { existing->Detail = std::string(detail); changed = true; }
                if (changed) ++s.version;
                return *existing;
            }

            auto now = std::chrono::system_clock::now();
            auto alarm = std::make_shared<AlarmData>();
            alarm->Code = code;
            alarm->Level = def.Level;
            alarm->Source = source.empty() ? SourceFromCode(code) : std::string(source);
            alarm->MessageVi = def.Vi;
            alarm->MessageEn = def.En;
            alarm->Cause = def.Cause;
            alarm->Remedy = def.Remedy;
            alarm->CauseEn = def.CauseEn;
            alarm->RemedyEn = def.RemedyEn;
            alarm->Detail = std::string(detail);
            alarm->RaisedAt = now;
            alarm->ConditionActive = true;
            alarm->IsRootCause = s.lastRaise == TimePoint{} || (now - s.lastRaise) > RootCauseWindow;

            s.lastRaise = now;
            s.active.push_back(alarm);
            s.history.push_back(alarm);
            while ((int)s.history.size() > s.maxHistory && !s.history.empty()) s.history.pop_front();
            ++s.version;
            copy = *alarm;
        }

        LogWorker::Instance().RecordLog(
            std::format("ALARM {} [{}] {}", code, ToString(copy.Level), copy.Message()) +
            (detail.empty() ? std::string() : " — " + std::string(detail)) +
            (copy.IsRootCause ? "  (root cause)" : ""),
            copy.Level >= EAlarmLevel::Error ? LogLevel::Error : LogLevel::Warning,
            copy.Source);

        AppendCsv(copy);
        s.alarmRaised.Raise(copy);
        return copy;
    }

    AlarmData AlarmManager::RaiseAxis(std::string_view axisName, AxisFault fault, std::string_view detail)
    {
        return Raise(AlarmCode::ForAxis(axisName, fault), axisName, detail);
    }

    AlarmData AlarmManager::RaiseEvent(int code, std::string_view source, std::string_view detail)
    {
        AlarmData alarm = Raise(code, source, detail);
        ClearCondition(code);
        alarm.ConditionActive = false;
        return alarm;
    }

    AlarmData AlarmManager::RaiseAxisEvent(std::string_view axisName, AxisFault fault, std::string_view detail)
    {
        return RaiseEvent(AlarmCode::ForAxis(axisName, fault), axisName, detail);
    }

    void AlarmManager::ClearCondition(int code)
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        AlarmPtr alarm = FindActive(s, code);
        if (alarm && alarm->ConditionActive)
        {
            alarm->ConditionActive = false;
            ++s.version;
        }
    }

    void AlarmManager::Dismiss(int code)
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        AlarmPtr alarm = FindActive(s, code);
        if (!alarm) return;
        EraseActive(s, code);
        alarm->ConditionActive = false;
        alarm->ClearedAt = std::chrono::system_clock::now();
        if (s.active.empty()) s.lastRaise = TimePoint{};
        ++s.version;
    }

    std::optional<AlarmData> AlarmManager::PendingReset()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        const AlarmData* best = nullptr;
        for (auto& p : s.active)
        {
            const AlarmData& a = *p;
            if (a.ConditionActive) continue;
            if (best == nullptr) { best = &a; continue; }
            if (a.Level != best->Level) { if (a.Level > best->Level) best = &a; continue; }
            if (a.RaisedAt < best->RaisedAt) best = &a;
        }
        return best ? std::optional<AlarmData>(*best) : std::nullopt;
    }

    void AlarmManager::ClearConditionAxis(std::string_view axisName, AxisFault fault)
    {
        ClearCondition(AlarmCode::ForAxis(axisName, fault));
    }

    bool AlarmManager::IsActive(int code)
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        return FindActive(s, code) != nullptr;
    }

    std::optional<AlarmData> AlarmManager::Get(int code)
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        AlarmPtr a = FindActive(s, code);
        return a ? std::optional<AlarmData>(*a) : std::nullopt;
    }

    std::vector<AlarmData> AlarmManager::Reset()
    {
        auto& s = S();
        std::vector<AlarmData> removed, stuck;

        {
            std::lock_guard<std::mutex> lock(s.mutex);
            std::vector<AlarmPtr> keep;
            for (auto& a : s.active)
            {
                if (a->ConditionActive) { keep.push_back(a); continue; }
                a->ClearedAt = std::chrono::system_clock::now();
                removed.push_back(*a);
            }
            s.active = std::move(keep);
            for (auto& a : s.active) stuck.push_back(*a);

            // Xoá sạch rồi thì sự cố kế tiếp là một cụm MỚI, phải được đánh dấu là nguyên nhân gốc.
            // Không đặt lại mốc thì alarm đầu tiên sau RESET có thể bị gom nhầm vào cụm cũ.
            if (s.active.empty()) s.lastRaise = TimePoint{};
            if (!removed.empty()) ++s.version;
        }

        if (!removed.empty())
        {
            std::vector<std::string> codes;
            for (auto& a : removed) codes.push_back(std::to_string(a.Code));
            LogWorker::Instance().RecordLog(
                std::format("RESET: cleared {} alarm(s) ({})", removed.size(), base::Join(codes, ", ")),
                LogLevel::Info, "Alarm");
        }

        if (!stuck.empty())
        {
            std::vector<std::string> items;
            for (auto& a : stuck) items.push_back(std::format("{} {}", a.Code, a.Message()));
            LogWorker::Instance().RecordLog(
                "RESET: " + std::to_string(stuck.size()) + " alarm(s) still active (cause not cleared) — " + base::Join(items, "; "),
                LogLevel::Warning, "Alarm");
        }

        return stuck;
    }

    void AlarmManager::ForceClearAll()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.active.clear();
        s.lastRaise = TimePoint{};
        ++s.version;
    }

    void AlarmManager::FlushCsv()
    {
        try
        {
            AlarmData a;
            while (PopCsv(a)) WriteCsvLine(a);
        }
        catch (...) { }
    }
}
