#include "Communication/GlobalVarSync.h"

#include "Base/Text.h"
#include "Communication/VmContract.h"

#include <algorithm>
#include <exception>
#include <set>

namespace scs
{
    namespace
    {
        const char* const NL = "\r\n";   // StringBuilder.AppendLine trên Windows
        const char* const ForbiddenChars = "?;./\\`{}[]<>()";

        std::string TrimEnd(std::string s)
        {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
            return s;
        }

        /// VisionMaster có thể trả tên bọc %tên% (dấu "không dịch" của giao diện).
        std::map<std::string, std::string> Normalize(const std::map<std::string, std::string>& raw)
        {
            std::map<std::string, std::string> d;
            for (auto& kv : raw)
            {
                std::string name = kv.first;
                if (name.size() > 1 && name.front() == '%' && name.back() == '%') name = name.substr(1, name.size() - 2);
                if (!name.empty()) d[name] = base::Trim(kv.second);
            }
            return d;
        }

        void AppendList(std::string& sb, const std::vector<GlobalVarSyncItem>& items, const char* title, EGlobalVarState state, bool withDetail)
        {
            std::vector<const GlobalVarSyncItem*> list;
            for (auto& i : items) if (i.State == state) list.push_back(&i);
            if (list.empty()) return;

            const size_t max = 15;
            std::vector<std::string> shown;
            for (size_t k = 0; k < list.size() && k < max; k++)
                shown.push_back(withDetail && !list[k]->Detail.empty() ? list[k]->Spec.Name + " (" + list[k]->Detail + ")" : list[k]->Spec.Name);
            sb += std::string(title) + ": " + base::Join(shown, ", ");
            if (list.size() > max) sb += " … and " + std::to_string(list.size() - max) + " more";
            sb += NL;
        }
    }

    int GlobalVarSyncReport::Count(EGlobalVarState state) const
    {
        return static_cast<int>(std::count_if(Items.begin(), Items.end(), [state](const GlobalVarSyncItem& i) { return i.State == state; }));
    }

    GlobalVarSyncReport GlobalVarSyncReport::Fail(const std::string& error)
    {
        GlobalVarSyncReport r;
        r.Error = error;
        return r;
    }

    std::string GlobalVarSyncReport::ToText() const
    {
        std::string sb;
        if (!Error.empty()) sb += "ERROR: " + Error + NL;
        if (Items.empty()) return TrimEnd(sb);

        sb += "Existing, correct type: " + std::to_string(Count(EGlobalVarState::Existing));
        if (Applied) sb += " · Created: " + std::to_string(Count(EGlobalVarState::Created));
        sb += " · Missing: " + std::to_string(Count(EGlobalVarState::Missing));
        sb += " · Wrong type: " + std::to_string(Count(EGlobalVarState::WrongType));
        if (Count(EGlobalVarState::InvalidName) > 0) sb += " · Invalid name: " + std::to_string(Count(EGlobalVarState::InvalidName));
        if (Count(EGlobalVarState::Failed) > 0) sb += " · Create failed: " + std::to_string(Count(EGlobalVarState::Failed));
        sb += NL;
        if (Applied) sb += std::string(Saved ? "Solution saved." : "Solution not saved (no new variables or save failed).") + NL;

        AppendList(sb, Items, "Wrong type — App does not fix it, fix manually in VisionMaster", EGlobalVarState::WrongType, true);
        AppendList(sb, Items, "Invalid name", EGlobalVarState::InvalidName, true);
        AppendList(sb, Items, "Create failed", EGlobalVarState::Failed, true);
        AppendList(sb, Items, "Still missing", EGlobalVarState::Missing, false);
        return TrimEnd(sb);
    }

    bool GlobalVarSync::IsValidName(const std::string& name, std::string& why)
    {
        why.clear();
        if (base::IsBlank(name)) { why = "empty name"; return false; }
        if (base::Trim(name) != name) { why = "leading/trailing spaces"; return false; }
        if (name.size() + 1 > 126) { why = "longer than 125 bytes"; return false; }
        if (name.find_first_of(ForbiddenChars) != std::string::npos) { why = "contains forbidden chars? ; . / \\ ` { } [ ] < > ( )"; return false; }
        return true;
    }

    GlobalVarSyncReport GlobalVarSync::Run(IGlobalVarStore* store, const std::vector<GlobalVarSpec>& specs, bool apply)
    {
        GlobalVarSyncReport report;
        if (store == nullptr) return GlobalVarSyncReport::Fail("variable store or variable list missing");

        std::map<std::string, std::string> existing;
        try { existing = Normalize(store->ReadAll()); }
        catch (const std::exception& ex) { return GlobalVarSyncReport::Fail(std::string("cannot read global variable list — ") + ex.what()); }

        std::set<std::string> seen;
        for (auto& spec : specs)
        {
            if (!seen.insert(spec.Name).second) continue;
            GlobalVarSyncItem item;
            item.Spec = spec;
            std::string why;
            auto it = existing.find(spec.Name);
            if (!IsValidName(spec.Name, why)) { item.State = EGlobalVarState::InvalidName; item.Detail = why; }
            else if (it != existing.end())
            {
                if (base::EqualsIgnoreCase(it->second, spec.Type)) item.State = EGlobalVarState::Existing;
                else { item.State = EGlobalVarState::WrongType; item.Detail = "is " + it->second + ", need " + spec.Type; }
            }
            else item.State = EGlobalVarState::Missing;
            report.Items.push_back(item);
        }

        if (!apply) return report;
        report.Applied = true;

        std::vector<GlobalVarSyncItem*> attempted;
        for (auto& item : report.Items) if (item.State == EGlobalVarState::Missing) attempted.push_back(&item);
        for (auto* item : attempted)
        {
            try
            {
                store->Add(item->Spec.Name, item->Spec.Type, item->Spec.Remark);
                if (!item->Spec.InitialValue.empty())
                    store->SetValue(item->Spec.Name, item->Spec.Type, item->Spec.InitialValue);
            }
            catch (const std::exception& ex)
            {
                item->State = EGlobalVarState::Failed;
                item->Detail = ex.what();
            }
        }

        std::map<std::string, std::string> after;
        try { after = Normalize(store->ReadAll()); }
        catch (const std::exception& ex)
        {
            for (auto* item : attempted)
            {
                if (item->State != EGlobalVarState::Missing) continue;
                item->State = EGlobalVarState::Failed;
                item->Detail = "cannot re-read variable list to confirm";
            }
            report.Error = std::string("cannot re-read variable list after create — ") + ex.what();
            return report;
        }

        for (auto* item : attempted)
        {
            if (item->State != EGlobalVarState::Missing) continue;
            auto it = after.find(item->Spec.Name);
            if (it != after.end() && base::EqualsIgnoreCase(it->second, item->Spec.Type))
                item->State = EGlobalVarState::Created;
            else
            {
                item->State = EGlobalVarState::Failed;
                item->Detail = it == after.end() ? std::string("variable not found after create") : "created as type " + it->second;
            }
        }

        if (report.Count(EGlobalVarState::Created) > 0)
        {
            try { store->Save(); report.Saved = true; }
            catch (const std::exception& ex) { report.Error = std::string("variables created but solution save failed — ") + ex.what(); }
        }
        return report;
    }

    std::vector<GlobalVarSpec> VisionGlobalVarCatalog::Build(const SystemData* sys)
    {
        return Build(sys != nullptr ? sys->VisionOutputs : VisionOutputNames());
    }

    std::vector<GlobalVarSpec> VisionGlobalVarCatalog::Build(const VisionOutputNames& names)
    {
        std::vector<GlobalVarSpec> list;

        auto Add = [&list](const std::string& name, const std::string& remark)
        {
            if (name.empty() || std::any_of(list.begin(), list.end(), [&](const GlobalVarSpec& s) { return s.Name == name; })) return;
            list.push_back(GlobalVarSpec{ name, "float", "0", remark, EGlobalVarGroup::AppWrites });
        };

        const std::string every = "App writes before every Flow";
        Add(names.GvAxisX, every + " — X axis (encoder at capture)");
        Add(names.GvAxisZ, every + " — Z axis (encoder at capture)");
        Add(names.GvNozzle, every + " — nozzle in front of CAM (1…7)");
        Add(VmVars::RunId, every + " — call number, echoed by Flow");
        if (!names.GvIsLocate.empty() && std::none_of(list.begin(), list.end(), [&](const GlobalVarSpec& s) { return s.Name == names.GvIsLocate; }))
            list.push_back(GlobalVarSpec{ names.GvIsLocate, "int", "1",
                                          every + " — 1 inspect, 0 load calib points (Branch module takes int)", EGlobalVarGroup::AppWrites });

        return list;
    }
}
