#include "EtherCat/Esi/EsiCatalog.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>

#include <windows.h>

#include "Base/Text.h"
#include "pugixml/pugixml.hpp"

namespace ecm
{
    namespace
    {
        namespace fs = std::filesystem;
        using pugi::xml_node;

        /// So sánh chuỗi theo culture hiện hành như Comparer<string>.Default của OrderBy bên C#.
        int CultureCompare(const std::string& a, const std::string& b)
        {
            std::wstring wa = base::Widen(a), wb = base::Widen(b);
            int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, 0, wa.c_str(), static_cast<int>(wa.size()),
                                    wb.c_str(), static_cast<int>(wb.size()), nullptr, nullptr, 0);
            return r == 0 ? a.compare(b) : r - CSTR_EQUAL;
        }

        /// Directory.GetFiles(folder, "*.xml", AllDirectories): file của thư mục trước, rồi tới từng
        /// thư mục con (đệ quy), mỗi cấp theo thứ tự tên như NTFS liệt kê.
        void CollectXmlFiles(const fs::path& dir, std::vector<fs::path>& out)
        {
            std::vector<fs::path> files, dirs;
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code ec2;
                if (it->is_directory(ec2)) dirs.push_back(it->path());
                else if (base::EqualsIgnoreCase(base::PathToU8(it->path().extension()), ".xml")) files.push_back(it->path());
            }
            auto byName = [](const fs::path& a, const fs::path& b)
            {
                return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
            };
            std::sort(files.begin(), files.end(), byName);
            std::sort(dirs.begin(), dirs.end(), byName);
            out.insert(out.end(), files.begin(), files.end());
            for (const auto& d : dirs)
                CollectXmlFiles(d, out);
        }

        std::vector<fs::path> XmlFilesUnder(const std::string& folderPath)
        {
            std::vector<fs::path> files;
            fs::path dir = base::U8Path(folderPath);
            std::error_code ec;
            if (folderPath.empty() || !fs::is_directory(dir, ec))
                return files;
            CollectXmlFiles(dir, files);
            return files;
        }

        /// XElement.Value: nối mọi text/CDATA con cháu.
        void AppendValue(const xml_node& node, std::string& out)
        {
            for (xml_node c : node.children())
            {
                if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) out += c.value();
                else if (c.type() == pugi::node_element) AppendValue(c, out);
            }
        }

        /// element?.Value — chuỗi rỗng khi phần tử không tồn tại (null của C# được xử lý như rỗng
        /// ở mọi chỗ gọi).
        std::string Value(const xml_node& node)
        {
            std::string s;
            if (node) AppendValue(node, s);
            return s;
        }

        /// <summary>
        /// ESI numeric attributes/elements appear as either plain decimal ("17185", "1") or
        /// "#x"-prefixed hex ("#xF0", "#x0800") — inconsistently, even within the same vendor's own
        /// files. Handle both. Ném exception với chuỗi sai dạng/tràn (như Convert.ToUInt32 /
        /// uint.Parse) — bên gọi bỏ qua cả file.
        /// </summary>
        uint32_t ParseNumeric(std::string_view rawIn)
        {
            if (base::IsBlank(rawIn))
                return 0;
            std::string raw = base::Trim(rawIn);
            uint64_t value = 0;
            if (raw.size() >= 2 && (raw[0] == '#' || raw[0] == '0') && (raw[1] == 'x' || raw[1] == 'X'))
            {
                std::string_view hex = std::string_view(raw).substr(2);
                if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex.remove_prefix(2);
                if (hex.empty()) throw std::invalid_argument("ESI: empty hex number");
                for (char c : hex)
                {
                    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                    if (d < 0) throw std::invalid_argument("ESI: bad hex number");
                    value = value * 16 + d;
                    if (value > 0xFFFFFFFFull) throw std::out_of_range("ESI: hex number overflow");
                }
                return static_cast<uint32_t>(value);
            }
            size_t i = 0;
            bool negative = false;
            if (raw[i] == '+' || raw[i] == '-') { negative = raw[i] == '-'; ++i; }
            if (i >= raw.size()) throw std::invalid_argument("ESI: bad number");
            for (; i < raw.size(); ++i)
            {
                if (raw[i] < '0' || raw[i] > '9') throw std::invalid_argument("ESI: bad number");
                value = value * 10 + (raw[i] - '0');
                if (value > 0xFFFFFFFFull) throw std::out_of_range("ESI: number overflow");
            }
            if (negative && value != 0) throw std::out_of_range("ESI: negative number");
            return static_cast<uint32_t>(value);
        }

        /// int.TryParse (NumberStyles.Integer): khoảng trắng đầu/cuối, dấu tuỳ chọn, chỉ chữ số thập phân.
        bool TryParseInt(std::string_view sIn, int& result)
        {
            std::string s = base::Trim(sIn);
            size_t i = 0;
            bool negative = false;
            if (!s.empty() && (s[0] == '+' || s[0] == '-')) { negative = s[0] == '-'; ++i; }
            if (i >= s.size()) return false;
            long long v = 0;
            for (; i < s.size(); ++i)
            {
                if (s[i] < '0' || s[i] > '9') return false;
                v = v * 10 + (s[i] - '0');
                if (v > 2147483648ll) return false;
            }
            if (negative) v = -v;
            if (v > 2147483647ll) return false;
            result = static_cast<int>(v);
            return true;
        }

        /// <summary>ESI files may declare several localized &lt;Name LcId="..."&gt; entries for the
        /// same element. Prefer English (LcId 1033); fall back to whichever one is present.</summary>
        std::string PreferEnglishName(const xml_node& parent)
        {
            xml_node first, english;
            for (xml_node e : parent.children("Name"))
            {
                if (!first) first = e;
                pugi::xml_attribute lc = e.attribute("LcId");
                if (lc && std::strcmp(lc.value(), "1033") == 0) { english = e; break; }
            }
            return base::Trim(Value(english ? english : first));
        }

        /// <summary>Pick the one PDO of pdoElementName that actually belongs in the process image,
        /// and read its mapping. A module typically declares two mutually exclusive variants of the
        /// same channels — a bit-per-entry one carrying the Sm attribute and a packed-word one
        /// without it — so summing all of them would double-count. The Sm-attributed variant is the
        /// ESI's own default assignment, so prefer it (alternate = the first one WITHOUT Sm).</summary>
        std::optional<EsiPdoInfo> SelectPdo(const xml_node& module, const char* pdoElementName, bool alternate = false)
        {
            xml_node first, withSm, withoutSm;
            for (xml_node p : module.children(pdoElementName))
            {
                if (!first) first = p;
                if (p.attribute("Sm")) { if (!withSm) withSm = p; }
                else if (!withoutSm) withoutSm = p;
            }
            if (!first)
                return std::nullopt;

            xml_node chosen = alternate ? withoutSm : (withSm ? withSm : first);
            if (!chosen)
                return std::nullopt;

            pugi::xml_attribute fixedAttr = chosen.attribute("Fixed");
            std::string fixedText = fixedAttr ? base::Trim(fixedAttr.value()) : std::string();
            pugi::xml_attribute smAttr = chosen.attribute("Sm");
            int sm = -1;

            EsiPdoInfo info;
            info.Index = static_cast<int>(ParseNumeric(Value(chosen.child("Index"))));
            info.SyncManager = (smAttr && TryParseInt(smAttr.value(), sm)) ? sm : -1;
            info.IsFixed = fixedAttr && (fixedText == "1" || base::EqualsIgnoreCase(fixedText, "true"));

            for (xml_node entry : chosen.children("Entry"))
            {
                int bitLen = 0;
                xml_node bitLenEl = entry.child("BitLen");
                if (!bitLenEl || !TryParseInt(Value(bitLenEl), bitLen))
                    continue;
                EsiPdoEntryInfo e;
                e.Index = static_cast<int>(ParseNumeric(Value(entry.child("Index"))));
                int sub = 0;
                xml_node subEl = entry.child("SubIndex");
                e.SubIndex = (subEl && TryParseInt(Value(subEl), sub)) ? sub : 0;
                e.BitLength = bitLen;
                info.Entries.push_back(e);
            }
            return info;
        }

        /// XDocument.Load(path).Root + kiểm tra LocalName == "EtherCATInfo". Ném khi XML hỏng (bên
        /// gọi bỏ qua file). Trả node rỗng khi gốc không phải ESI, hoặc khi gốc khai báo default
        /// namespace (C# tra Element("Vendor") không namespace nên khi đó không tìm thấy gì).
        xml_node LoadRoot(const fs::path& path, pugi::xml_document& doc)
        {
            pugi::xml_parse_result r = doc.load_file(path.c_str());
            if (!r) throw std::runtime_error(r.description());
            xml_node root = doc.document_element();
            if (!root) return xml_node();
            std::string_view name = root.name();
            if (size_t colon = name.find(':'); colon != std::string_view::npos) name.remove_prefix(colon + 1);
            if (name != "EtherCATInfo") return xml_node();
            pugi::xml_attribute ns = root.attribute("xmlns");
            if (ns && ns.value()[0] != '\0') return xml_node();
            return root;
        }

        std::string AttrText(const xml_node& node, const char* name)
        {
            pugi::xml_attribute a = node.attribute(name);
            return a ? std::string(a.value()) : std::string();
        }

        void ParseSyncManagers(const fs::path& path, std::vector<EsiSyncManagerInfo>& list)
        {
            pugi::xml_document doc;
            xml_node root = LoadRoot(path, doc);
            if (!root) return;

            uint32_t vendorId = ParseNumeric(Value(root.child("Vendor").child("Id")));
            xml_node devices = root.child("Descriptions").child("Devices");

            for (xml_node device : devices.children("Device"))
            {
                xml_node typeEl = device.child("Type");
                if (!typeEl)
                    continue;
                uint32_t productCode = ParseNumeric(AttrText(typeEl, "ProductCode"));

                // Declaration order is the SyncManager index - that is how ESI identifies them,
                // there is no index attribute.
                int index = 0;
                for (xml_node sm : device.children("Sm"))
                {
                    pugi::xml_attribute enable = sm.attribute("Enable");
                    EsiSyncManagerInfo info;
                    info.VendorId = vendorId;
                    info.ProductCode = productCode;
                    info.Index = index++;
                    info.StartAddress = static_cast<int>(ParseNumeric(AttrText(sm, "StartAddress")));
                    info.ControlByte = static_cast<int>(ParseNumeric(AttrText(sm, "ControlByte")));
                    info.Enable = base::Trim(enable ? enable.value() : "0") != "0";
                    info.Purpose = base::Trim(Value(sm));
                    list.push_back(std::move(info));
                }
            }
        }

        void ParseModules(const fs::path& path, std::vector<EsiModuleInfo>& list)
        {
            pugi::xml_document doc;
            xml_node root = LoadRoot(path, doc);
            if (!root) return;

            xml_node descriptions = root.child("Descriptions");
            xml_node modules = descriptions.child("Modules");
            if (!modules)
                return;

            // A coupler ESI declares its Modules once, alongside the Device(s) it belongs to. Tag
            // each module with the coupler's product code so a lookup can be scoped to the coupler
            // actually found on the bus (two vendors could reuse an ident value).
            uint32_t couplerProductCode = ParseNumeric(
                AttrText(descriptions.child("Devices").child("Device").child("Type"), "ProductCode"));

            for (xml_node module : modules.children("Module"))
            {
                xml_node typeEl = module.child("Type");
                if (!typeEl)
                    continue;

                std::string typeName = base::Trim(Value(typeEl));
                std::string displayName = PreferEnglishName(module);

                EsiModuleInfo info;
                info.InputPdo = SelectPdo(module, "TxPdo");
                info.OutputPdo = SelectPdo(module, "RxPdo");
                // C#: ReferenceEquals(alt, primary) chỉ đúng khi cả hai null → Alt luôn là kết quả
                // SelectPdo(alternate) (kể cả khi trùng nội dung với PDO chính).
                info.InputPdoAlt = SelectPdo(module, "TxPdo", true);
                info.OutputPdoAlt = SelectPdo(module, "RxPdo", true);
                info.ModuleIdent = ParseNumeric(AttrText(typeEl, "ModuleIdent"));
                info.TypeName = typeName;
                info.DisplayName = displayName.empty() ? typeName : displayName;
                info.ModuleClass = base::Trim(AttrText(typeEl, "ModuleClass"));
                info.CouplerProductCode = couplerProductCode;
                info.InputBits = info.InputPdo ? info.InputPdo->TotalBits() : 0;
                info.OutputBits = info.OutputPdo ? info.OutputPdo->TotalBits() : 0;
                list.push_back(std::move(info));
            }
        }

        void ParseFile(const fs::path& path, std::vector<EsiDeviceInfo>& list)
        {
            pugi::xml_document doc;
            xml_node root = LoadRoot(path, doc);
            if (!root) return;

            xml_node vendorEl = root.child("Vendor");
            uint32_t vendorId = ParseNumeric(Value(vendorEl.child("Id")));
            std::string vendorName = PreferEnglishName(vendorEl);
            xml_node devices = root.child("Descriptions").child("Devices");

            for (xml_node device : devices.children("Device"))
            {
                xml_node typeEl = device.child("Type");
                if (!typeEl)
                    continue;

                std::string typeName = base::Trim(Value(typeEl));
                std::string displayName = PreferEnglishName(device);

                EsiDeviceInfo info;
                info.VendorId = vendorId;
                info.VendorName = vendorName;
                info.ProductCode = ParseNumeric(AttrText(typeEl, "ProductCode"));
                info.RevisionNo = ParseNumeric(AttrText(typeEl, "RevisionNo"));
                info.TypeName = typeName;
                info.DisplayName = displayName.empty() ? typeName : displayName;
                info.GroupType = base::Trim(Value(device.child("GroupType")));
                info.SourceFile = base::PathToU8(path.filename());
                list.push_back(std::move(info));
            }
        }

        /// Một file hỏng/lạ không được chặn phần còn lại của catalog: phần đã đọc dở của file đó bị
        /// bỏ (C# AddRange chỉ chạy khi cả file parse xong), các file khác vẫn nạp.
        template <class T, class ParseFn>
        std::vector<T> LoadAll(const std::string& folderPath, ParseFn parse)
        {
            std::vector<T> result;
            for (const auto& file : XmlFilesUnder(folderPath))
            {
                std::vector<T> one;
                try
                {
                    parse(file, one);
                }
                catch (...)
                {
                    // Not a valid ESI file, or an unexpected schema variant — skip it, don't abort the catalog.
                    continue;
                }
                for (auto& item : one)
                    result.push_back(std::move(item));
            }
            return result;
        }
    }

    std::vector<EsiDeviceInfo> EsiCatalog::LoadFromFolder(const std::string& folderPath)
    {
        return LoadAll<EsiDeviceInfo>(folderPath, ParseFile);
    }

    std::vector<EsiModuleInfo> EsiCatalog::LoadModulesFromFolder(const std::string& folderPath)
    {
        return LoadAll<EsiModuleInfo>(folderPath, ParseModules);
    }

    std::vector<EsiSyncManagerInfo> EsiCatalog::LoadSyncManagersFromFolder(const std::string& folderPath)
    {
        return LoadAll<EsiSyncManagerInfo>(folderPath, ParseSyncManagers);
    }

    std::vector<EsiVendorInfo> EsiCatalog::LoadVendorsFromFolder(const std::string& folderPath)
    {
        // GroupBy(VendorId): nhóm theo thứ tự xuất hiện đầu tiên.
        std::vector<EsiVendorInfo> vendors;
        for (auto& d : LoadFromFolder(folderPath))
        {
            auto it = std::find_if(vendors.begin(), vendors.end(), [&](const EsiVendorInfo& v) { return v.VendorId == d.VendorId; });
            if (it == vendors.end())
            {
                EsiVendorInfo v;
                v.VendorId = d.VendorId;
                vendors.push_back(std::move(v));
                it = vendors.end() - 1;
            }
            if (it->VendorName.empty() && !d.VendorName.empty())
                it->VendorName = d.VendorName;
            it->Drives.push_back(std::move(d));
        }
        for (auto& v : vendors)
            std::stable_sort(v.Drives.begin(), v.Drives.end(),
                             [](const EsiDeviceInfo& a, const EsiDeviceInfo& b) { return CultureCompare(a.DisplayName, b.DisplayName) < 0; });
        std::stable_sort(vendors.begin(), vendors.end(),
                         [](const EsiVendorInfo& a, const EsiVendorInfo& b) { return CultureCompare(a.VendorName, b.VendorName) < 0; });
        return vendors;
    }
}
