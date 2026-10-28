#include "Data/McDataManager.h"

#include "Base/Text.h"
#include "Infor/AlarmManager.h"
#include "Infor/AxisId.h"
#include "SysConfig/AppPaths.h"
#include "Utility/Log/LogWorker.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <stdexcept>

namespace scs
{
    namespace fs = std::filesystem;

    namespace
    {
        struct State
        {
            std::mutex mutex;                 // ba con trỏ + modelLoaded
            std::shared_ptr<SystemData> sys = SystemData::CreateDefault();
            std::shared_ptr<RunningData> run = std::make_shared<RunningData>();
            std::shared_ptr<ModelData> model = std::make_shared<ModelData>();
            bool modelLoaded{ false };
            std::recursive_mutex saveLock;
        };

        State& S()
        {
            static State s;
            return s;
        }

        template <class T> void Set(std::shared_ptr<T> State::* member, std::shared_ptr<T> value)
        {
            auto& s = S();
            std::lock_guard<std::mutex> lock(s.mutex);
            s.*member = std::move(value);
        }

        void SetModel(std::shared_ptr<ModelData> model, bool loaded)
        {
            auto& s = S();
            std::lock_guard<std::mutex> lock(s.mutex);
            s.model = std::move(model);
            s.modelLoaded = loaded;
        }

        std::string ReadFile(const fs::path& path)
        {
            std::string text;
            if (!base::ReadAllText(path, text)) throw std::runtime_error("Cannot read " + base::PathToU8(path));
            return text;
        }

        /// <summary>Ghi qua file tạm rồi thay thế. Mất điện giữa lúc ghi sẽ để lại file cũ nguyên vẹn
        /// thay vì một file JSON cụt — với running.json đó là khác biệt giữa "mất tiến độ một ô" và
        /// "mất cả tray".</summary>
        void WriteAtomic(const fs::path& path, const std::string& content)
        {
            std::lock_guard<std::recursive_mutex> lock(S().saveLock);
            if (path.has_parent_path()) fs::create_directories(path.parent_path());

            fs::path tmp = path;
            tmp += ".tmp";
            if (!base::WriteAllText(tmp, content)) throw std::runtime_error("Cannot write " + base::PathToU8(tmp));

            if (fs::exists(path)) fs::remove(path);
            fs::rename(tmp, path);
        }

        fs::path ModelPath(const std::string& name)
        {
            return AppPaths::ModelDir() / base::U8Path(name + ".json");
        }

        void Log(const std::string& msg, LogLevel level = LogLevel::Info)
        {
            LogWorker::Instance().RecordLog(msg, level, "Data");
        }
    }

    std::shared_ptr<SystemData> McDataManager::SysData()  { auto& s = S(); std::lock_guard<std::mutex> l(s.mutex); return s.sys; }
    std::shared_ptr<RunningData> McDataManager::RunData() { auto& s = S(); std::lock_guard<std::mutex> l(s.mutex); return s.run; }
    std::shared_ptr<ModelData> McDataManager::Model()     { auto& s = S(); std::lock_guard<std::mutex> l(s.mutex); return s.model; }
    bool McDataManager::ModelLoaded()                     { auto& s = S(); std::lock_guard<std::mutex> l(s.mutex); return s.modelLoaded; }

    // ───────────────────────── system.json ─────────────────────────

    void McDataManager::LoadSystemData()
    {
        try
        {
            if (fs::exists(AppPaths::SystemFile()))
            {
                auto loaded = SystemData::FromJsonText(ReadFile(AppPaths::SystemFile()));
                if (loaded)
                {
                    // Sắp lại theo đúng AxisId::All — vừa bổ sung trục còn thiếu, vừa bỏ trục lạ, vừa tự
                    // sửa thứ tự; không cần xoá system.json thủ công.
                    std::vector<std::shared_ptr<AxisConfig>> axes;
                    for (const char* name : AxisId::All) axes.push_back(loaded->GetAxis(name));
                    loaded->Axes = std::move(axes);

                    // Máy dùng Panasonic MINAS A6B. File còn giữ mặc định cũ Leadshine L5N (0x4321/0xF0 — system.json
                    // chép từ máy khác) thì đổi sang mặc định mới; driver đã Scan (mã khác) thì giữ nguyên.
                    {
                        auto defaults = SystemData::CreateDefault();
                        bool migrated = false;
                        for (const char* name : AxisId::All)
                        {
                            auto a = loaded->GetAxis(name);
                            if (!a || a->ExpectedVendorId != 0x4321u || a->ExpectedProductCode != 0xF0u) continue;
                            auto d = defaults->GetAxis(name);
                            a->ExpectedVendorId = d->ExpectedVendorId;
                            a->ExpectedProductCode = d->ExpectedProductCode;
                            a->ExpectedRevision = d->ExpectedRevision;
                            a->ExpectedDriverName = d->ExpectedDriverName;
                            migrated = true;
                        }
                        if (migrated)
                            Log("system.json: axis driver changed from Leadshine L5N to Panasonic MINAS A6B — "
                                "Scan on EtherCAT page to set the exact product code, then Save", LogLevel::Warning);
                    }
                    if (!loaded->IoModule) loaded->IoModule = SystemData::DefaultIoModule();
                    Set(&State::sys, loaded);

                    Log("system.json loaded");
                    return;
                }
            }

            Set(&State::sys, SystemData::CreateDefault());
            SaveSystemData();
            Log("system.json not found — default created", LogLevel::Warning);
        }
        catch (const std::exception& ex)
        {
            Set(&State::sys, SystemData::CreateDefault());
            AlarmManager::Raise(AlarmCode::ConfigInvalid, "Data", ex.what());
        }
    }

    bool McDataManager::SaveSystemData()
    {
        try
        {
            WriteAtomic(AppPaths::SystemFile(), SysData()->ToJsonText());
            return true;
        }
        catch (const std::exception& ex)
        {
            AlarmManager::RaiseEvent(AlarmCode::ConfigInvalid, "Data", std::string("system.json save failed: ") + ex.what());
            return false;
        }
    }

    // ───────────────────────── running.json ─────────────────────────

    void McDataManager::LoadRunningData()
    {
        try
        {
            if (fs::exists(AppPaths::RunningFile()))
            {
                auto loaded = RunningData::FromJsonText(ReadFile(AppPaths::RunningFile()));
                if (loaded)
                {
                    // Không resume chu trình qua một lần mở lại app — bắt buộc Init lại toàn bộ máy mỗi
                    // lần khởi động. Bộ đếm OK/NG và model dùng gần nhất vẫn giữ: đó là số liệu báo cáo.
                    loaded->ResetCycleProgress();
                    Set(&State::run, loaded);

                    Log(std::format("running.json loaded (counter OK {}/NG {}) — "
                                    "cycle restarts from beginning, previous session progress discarded",
                                    loaded->CountOk, loaded->CountNg));
                    return;
                }
            }
            Set(&State::run, std::make_shared<RunningData>());
        }
        catch (const std::exception& ex)
        {
            Set(&State::run, std::make_shared<RunningData>());
            Log(std::string("running.json read failed: ") + ex.what(), LogLevel::Error);
        }
    }

    bool McDataManager::SaveRunningData()
    {
        try
        {
            auto run = RunData();
            run->SavedAt = std::chrono::system_clock::now();
            WriteAtomic(AppPaths::RunningFile(), run->ToJsonText());
            AlarmManager::ClearCondition(AlarmCode::RunningDataFailed);
            return true;
        }
        catch (const std::exception& ex)
        {
            AlarmManager::Raise(AlarmCode::RunningDataFailed, "Data", ex.what());
            return false;
        }
    }

    // ───────────────────────── model ─────────────────────────

    std::vector<std::string> McDataManager::ListModels()
    {
        std::vector<std::string> names;
        try
        {
            for (const auto& e : fs::directory_iterator(AppPaths::ModelDir()))
            {
                if (!e.is_regular_file()) continue;
                if (!base::EqualsIgnoreCase(base::PathToU8(e.path().extension()), ".json")) continue;
                names.push_back(base::PathToU8(e.path().stem()));
            }
            std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
                std::string la = base::ToLower(a), lb = base::ToLower(b);
                return la != lb ? la < lb : a < b;
            });
        }
        catch (...) { names.clear(); }
        return names;
    }

    bool McDataManager::LoadModel(const std::string& name)
    {
        fs::path path = ModelPath(name);
        try
        {
            if (!fs::exists(path))
            {
                AlarmManager::RaiseEvent(AlarmCode::ModelFileCorrupt, "Data", "Not found: " + base::PathToU8(path));
                return false;
            }

            auto loaded = ModelData::FromJsonText(ReadFile(path));
            if (!loaded)
            {
                AlarmManager::RaiseEvent(AlarmCode::ModelFileCorrupt, "Data", base::PathToU8(path));
                return false;
            }

            // Model tạo trước khi có quy trình Auto: bổ sung các điểm teach chu trình cần (chưa có toạ độ).
            int added = loaded->EnsureAutoPoints();
            SetModel(loaded, true);
            RunData()->RecentModelName = name;

            Log(std::format("Model '{}' loaded: {} teach points{}", name, loaded->TeachPositions.size(),
                            added > 0 ? std::format(" (added {} Auto cycle points — teach, then Save)", added) : std::string()));
            return true;
        }
        catch (const std::exception& ex)
        {
            AlarmManager::RaiseEvent(AlarmCode::ModelFileCorrupt, "Data", ex.what());
            return false;
        }
    }

    void McDataManager::UseModel(std::shared_ptr<ModelData> model, bool countsAsLoaded)
    {
        if (!model) return;
        SetModel(std::move(model), countsAsLoaded);
    }

    bool McDataManager::SaveModel(const std::string& nameArg)
    {
        auto model = Model();
        std::string name = nameArg.empty() ? model->Name : nameArg;
        try
        {
            model->Name = name;
            model->ModifiedAt = std::chrono::system_clock::now();
            WriteAtomic(ModelPath(name), model->ToJsonText());
            Log("Model saved: " + name);
            return true;
        }
        catch (const std::exception& ex)
        {
            AlarmManager::RaiseEvent(AlarmCode::ModelSaveFailed, "Data", ex.what());
            return false;
        }
    }

    bool McDataManager::DeleteModel(const std::string& name)
    {
        try
        {
            fs::path path = ModelPath(name);
            if (fs::exists(path)) fs::remove(path);
            Log("Model deleted: " + name);
            return true;
        }
        catch (const std::exception& ex)
        {
            AlarmManager::RaiseEvent(AlarmCode::ModelSaveFailed, "Data", std::string("Model delete failed: ") + ex.what());
            return false;
        }
    }

    bool McDataManager::CopyModel(const std::string& sourceName, const std::string& newName)
    {
        try
        {
            auto data = ModelData::FromJsonText(ReadFile(ModelPath(sourceName)));
            if (!data) return false;

            data->Name = newName;
            data->ModifiedAt = std::chrono::system_clock::now();
            WriteAtomic(ModelPath(newName), data->ToJsonText());

            Log("Model copied: " + sourceName + " → " + newName);
            return true;
        }
        catch (const std::exception& ex)
        {
            AlarmManager::RaiseEvent(AlarmCode::ModelSaveFailed, "Data", std::string("Model copy failed: ") + ex.what());
            return false;
        }
    }

    void McDataManager::LoadRecentModel()
    {
        std::string recent = RunData()->RecentModelName;
        if (!base::IsBlank(recent) && LoadModel(recent)) return;

        auto all = ListModels();
        if (!all.empty() && LoadModel(all[0])) return;

        SetModel(ModelData::CreateDefault(), false);
        SaveModel("DEFAULT");
        LoadModel("DEFAULT");
    }
}
