#pragma once
// Đọc JSON kiểu Newtonsoft: thiếu khoá / null → giữ giá trị mặc định, khoá lạ bỏ qua, sai kiểu ở
// một trường thì chỉ bỏ trường đó. Chỉ include từ file .cpp (json.hpp nặng).
#include "Utility/DateTimeText.h"
#include "nlohmann/json.hpp"

#include <optional>
#include <string>

namespace scs::jsonx
{
    using json = nlohmann::ordered_json;

    template <class T>
    void Get(const json& j, const char* key, T& field)
    {
        auto it = j.find(key);
        if (it == j.end() || it->is_null()) return;
        try { field = it->template get<T>(); } catch (...) { }
    }

    template <class T>
    void Get(const json& j, const char* key, std::optional<T>& field)
    {
        auto it = j.find(key);
        if (it == j.end()) return;
        if (it->is_null()) { field.reset(); return; }
        try { field = it->template get<T>(); } catch (...) { }
    }

    inline void GetTime(const json& j, const char* key, TimePoint& field)
    {
        auto it = j.find(key);
        if (it == j.end() || !it->is_string()) return;
        TimePoint t;
        if (TryParseDateTime(it->get<std::string>(), t)) field = t;
    }

    inline std::string Dump(const json& j)
    {
        return j.dump(2, ' ', false, json::error_handler_t::replace);
    }

    /// <summary>JsonConvert.DeserializeObject: chuỗi rỗng hoặc "null" → false (đối tượng null);
    /// JSON hỏng hoặc gốc không phải object → ném.</summary>
    inline bool ParseObject(const std::string& text, json& root)
    {
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) return false;
        root = json::parse(text);
        if (root.is_null()) return false;
        if (!root.is_object()) throw std::runtime_error("JSON: root is not an object");
        return true;
    }
}
