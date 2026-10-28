#include "Data/Model/ModelData.h"

#include "Base/Text.h"
#include "Infor/AxisId.h"
#include "Utility/JsonHelper.h"

#include <algorithm>
#include <format>

namespace scs
{
    using jsonx::json;

    bool TeachPosition::Has(std::string_view axis) const
    {
        for (auto& kv : Pos) if (kv.first == axis) return true;
        return false;
    }

    double TeachPosition::Get(std::string_view axis, double fallback) const
    {
        for (auto& kv : Pos) if (kv.first == axis) return kv.second;
        return fallback;
    }

    void TeachPosition::Set(std::string_view axis, double value)
    {
        for (auto& kv : Pos) if (kv.first == axis) { kv.second = value; return; }
        Pos.emplace_back(std::string(axis), value);
    }

    bool TeachPosition::Remove(std::string_view axis)
    {
        return std::erase_if(Pos, [&](const auto& kv) { return kv.first == axis; }) > 0;
    }

    std::shared_ptr<TeachPosition> ModelData::FindPosition(std::string_view name) const
    {
        for (auto& p : TeachPositions)
            if (p && base::EqualsIgnoreCase(p->Name, name)) return p;
        return nullptr;
    }

    // ───────────────────────── tên điểm ─────────────────────────

    std::string AutoPoints::PickRow(int tray, bool last) { return std::format("Pick Tray{} · {}", tray, last ? "Last row" : "Row 1"); }
    std::string AutoPoints::Cam(bool last) { return last ? "CAM · N7" : "CAM · N1"; }
    std::string AutoPoints::Laser(bool last) { return last ? "Laser · N7" : "Laser · N1"; }

    namespace
    {
        std::string PickRowNote(int tray, bool last)
        {
            return std::format("X, Y, Z: N1 over column 1 of {} of Tray{} (Z = pick height)", last ? "the last row" : "row 1", tray);
        }
        std::string CamNote(bool last) { return std::format("X, Z: N{} centered in CAM view", last ? 7 : 1); }
        std::string LaserNote(bool last) { return std::format("X, Z: N{} at laser marking center", last ? 7 : 1); }
    }

    bool AutoPoints::IsLayoutPoint(std::string_view name)
    {
        for (const auto& [n, note] : All())
            if (base::EqualsIgnoreCase(name, n)) return true;
        return false;
    }

    std::vector<std::pair<std::string, std::string>> AutoPoints::All()
    {
        std::vector<std::pair<std::string, std::string>> list;
        for (int t = 1; t <= TrayCount; ++t)
        {
            list.push_back({ PickRow(t, false), PickRowNote(t, false) });
            list.push_back({ PickRow(t, true), PickRowNote(t, true) });
        }
        list.push_back({ Cam(false), CamNote(false) });
        list.push_back({ Cam(true), CamNote(true) });
        list.push_back({ Laser(false), LaserNote(false) });
        list.push_back({ Laser(true), LaserNote(true) });
        return list;
    }

    // ───────────────────────── chia đều ─────────────────────────

    TeachPosition ModelData::Interpolate(const TeachPosition& first, const TeachPosition& last, int index, int count)
    {
        TeachPosition p = first;
        double t = count > 1 ? std::clamp(static_cast<double>(index) / (count - 1), 0.0, 1.0) : 0.0;
        for (auto& [axis, v] : p.Pos)
            if (last.Has(axis)) v = v * (1.0 - t) + last.Get(axis) * t;   // t = 0 / 1 ra đúng số của điểm đầu / cuối
        if (first.TaughtAt && last.TaughtAt) p.TaughtAt = std::max(*first.TaughtAt, *last.TaughtAt);
        return p;
    }

    namespace
    {
        /// <summary>Điểm thứ <paramref name="number"/> (1…count) chia đều giữa hai điểm teach. Điểm đầu phải teach;
        /// count &gt; 1 thì điểm cuối cũng phải teach.</summary>
        std::shared_ptr<TeachPosition> Between(const ModelData& m, const std::string& firstName, const std::string& lastName,
                                               int number, int count, std::string name, std::string& why)
        {
            if (count < 1 || number < 1 || number > count)
            {
                why = std::format("\"{}\" out of range 1…{} (Teaching page)", name, count);
                return nullptr;
            }
            auto first = m.FindPosition(firstName);
            if (!first || !first->Taught())
            {
                why = std::format("point \"{}\" not taught (Teaching page)", firstName);
                return nullptr;
            }
            auto last = first;
            if (count > 1)
            {
                last = m.FindPosition(lastName);
                if (!last || !last->Taught())
                {
                    why = std::format("point \"{}\" not taught (Teaching page)", lastName);
                    return nullptr;
                }
            }
            auto p = std::make_shared<TeachPosition>(ModelData::Interpolate(*first, *last, number - 1, count));
            p->Name = std::move(name);
            p->Note.clear();
            return p;
        }
    }

    std::shared_ptr<TeachPosition> ModelData::PickPoint(int tray, int row, std::string& why) const
    {
        if (tray < 1 || tray > AutoPoints::TrayCount) { why = std::format("Tray{} does not exist (Tray1, Tray2 only)", tray); return nullptr; }
        return Between(*this, AutoPoints::PickRow(tray, false), AutoPoints::PickRow(tray, true), row, std::max(1, Auto.TrayRows),
                       std::format("Pick Tray{} · R{}", tray, row), why);
    }

    std::shared_ptr<TeachPosition> ModelData::CamPoint(int nozzle, std::string& why) const
    {
        return Between(*this, AutoPoints::Cam(false), AutoPoints::Cam(true), nozzle, AutoPoints::NozzleCount,
                       std::format("CAM · N{}", nozzle), why);
    }

    std::shared_ptr<TeachPosition> ModelData::LaserPoint(int nozzle, std::string& why) const
    {
        return Between(*this, AutoPoints::Laser(false), AutoPoints::Laser(true), nozzle, AutoPoints::NozzleCount,
                       std::format("Laser · N{}", nozzle), why);
    }

    int ModelData::EnsureAutoPoints()
    {
        int added = 0;
        for (const auto& [name, note] : AutoPoints::All())
        {
            if (FindPosition(name)) continue;
            auto p = std::make_shared<TeachPosition>();
            p->Name = name;
            p->Note = note;
            TeachPositions.push_back(p);
            ++added;
        }
        return added;
    }

    std::shared_ptr<ModelData> ModelData::CreateDefault(std::string_view name)
    {
        auto m = std::make_shared<ModelData>();
        m->Name = std::string(name);
        m->EnsureAutoPoints();
        return m;
    }

    // ───────────────────────── JSON ─────────────────────────

    std::string ModelData::ToJsonText() const
    {
        json j = json::object();
        j["Name"] = Name;
        if (!Description.empty()) j["Description"] = Description;   // null → không ghi
        j["ModifiedAt"] = ToIsoString(ModifiedAt);
        json list = json::array();
        for (auto& p : TeachPositions)
        {
            if (!p) continue;
            json e = json::object();
            e["Name"] = p->Name;
            e["Note"] = p->Note;
            json pos = json::object();
            for (auto& kv : p->Pos) pos[kv.first] = kv.second;
            e["Pos"] = std::move(pos);
            if (p->TaughtAt) e["TaughtAt"] = ToIsoString(*p->TaughtAt);
            e["Taught"] = p->Taught();   // property chỉ-đọc: ghi để đọc file bằng mắt, khi đọc thì bỏ qua
            list.push_back(std::move(e));
        }
        j["TeachPositions"] = std::move(list);
        json a = json::object();
        a["TrayRows"] = Auto.TrayRows;
        a["VacuumOnMs"] = Auto.VacuumOnMs;
        a["VacuumOffMs"] = Auto.VacuumOffMs;
        a["PickSlowLength"] = Auto.PickSlowLength;
        a["UseVision"] = Auto.UseVision;
        a["UseLaser"] = Auto.UseLaser;
        a["MarkTimeMs"] = Auto.MarkTimeMs;
        j["Auto"] = std::move(a);

        json c = json::object();
        if (Calib.GridVerifiedAt) c["GridVerifiedAt"] = ToIsoString(*Calib.GridVerifiedAt);
        json nozzles = json::array();
        for (const auto& n : Calib.Nozzles)
        {
            json e = json::object();
            e["RefX"] = n.RefX;
            e["RefZ"] = n.RefZ;
            if (n.RefAt) e["RefAt"] = ToIsoString(*n.RefAt);
            e["TrimX"] = n.TrimX;
            e["TrimZ"] = n.TrimZ;
            nozzles.push_back(std::move(e));
        }
        c["Nozzles"] = std::move(nozzles);
        j["Calib"] = std::move(c);
        return jsonx::Dump(j);
    }

    std::shared_ptr<ModelData> ModelData::FromJsonText(const std::string& text)
    {
        json j;
        if (!jsonx::ParseObject(text, j)) return nullptr;

        auto m = std::make_shared<ModelData>();
        jsonx::Get(j, "Name", m->Name);
        jsonx::Get(j, "Description", m->Description);
        jsonx::GetTime(j, "ModifiedAt", m->ModifiedAt);
        if (auto it = j.find("TeachPositions"); it != j.end() && it->is_array())
        {
            for (const auto& e : *it)
            {
                if (!e.is_object()) continue;
                auto p = std::make_shared<TeachPosition>();
                jsonx::Get(e, "Name", p->Name);
                jsonx::Get(e, "Note", p->Note);
                if (auto pos = e.find("Pos"); pos != e.end() && pos->is_object())
                    for (auto kv = pos->begin(); kv != pos->end(); ++kv)
                        if (kv.value().is_number()) p->Set(kv.key(), kv.value().get<double>());
                TimePoint t;
                if (auto ta = e.find("TaughtAt"); ta != e.end() && ta->is_string() && TryParseDateTime(ta->get<std::string>(), t))
                    p->TaughtAt = t;
                m->TeachPositions.push_back(p);
            }
        }
        if (auto a = j.find("Auto"); a != j.end() && a->is_object())
        {
            jsonx::Get(*a, "TrayRows", m->Auto.TrayRows);
            jsonx::Get(*a, "VacuumOnMs", m->Auto.VacuumOnMs);
            jsonx::Get(*a, "VacuumOffMs", m->Auto.VacuumOffMs);
            jsonx::Get(*a, "PickSlowLength", m->Auto.PickSlowLength);
            jsonx::Get(*a, "UseVision", m->Auto.UseVision);
            jsonx::Get(*a, "UseLaser", m->Auto.UseLaser);
            jsonx::Get(*a, "MarkTimeMs", m->Auto.MarkTimeMs);
        }
        if (auto c = j.find("Calib"); c != j.end() && c->is_object())
        {
            auto time = [](const json& o, const char* key, std::optional<TimePoint>& out)
            {
                TimePoint t;
                if (auto it = o.find(key); it != o.end() && it->is_string() && TryParseDateTime(it->get<std::string>(), t)) out = t;
            };
            time(*c, "GridVerifiedAt", m->Calib.GridVerifiedAt);
            if (auto nozzles = c->find("Nozzles"); nozzles != c->end() && nozzles->is_array())
            {
                size_t i = 0;
                for (const auto& e : *nozzles)
                {
                    if (i >= m->Calib.Nozzles.size()) break;
                    auto& n = m->Calib.Nozzles[i++];
                    if (!e.is_object()) continue;
                    jsonx::Get(e, "RefX", n.RefX);
                    jsonx::Get(e, "RefZ", n.RefZ);
                    time(e, "RefAt", n.RefAt);
                    jsonx::Get(e, "TrimX", n.TrimX);
                    jsonx::Get(e, "TrimZ", n.TrimZ);
                }
            }
        }
        return m;
    }
}
