#include "Data/SystemData.h"

#include <cmath>
#include <cstdlib>

#include "Base/Text.h"
#include "Infor/AxisId.h"
#include "Utility/JsonHelper.h"

namespace scs
{
    using jsonx::json;
    using jsonx::Get;

    namespace
    {
        // Định danh Leadshine — bản sao của EtherCatMaster.Constants.LeadshineDrives (thư viện ecm). Chỉ còn Remote IO R1EC
        // là Leadshine.
        constexpr uint32_t LeadshineVendorId = 17185;          // 0x4321
        constexpr uint32_t R1ecProductCode = 0x61400015;
        constexpr uint32_t R1ecRevision = 0x22092010;
        // X, Y, Z: Panasonic MINAS A6B (EtherCAT). Định danh theo DriverInfo\Panasonic\panasonic_minas-a6be_v1_9.xml.
        // Product code theo TỪNG model driver (MADLN05BE 0x60380004, MADLN15BE 0x60380005, MBDLN25BE 0x60380006 …) —
        // chưa biết model thật nên để MADLN15BE; trên máy bấm Scan ở trang EtherCAT để gán đúng mã của driver lắp thực tế.
        constexpr uint32_t PanasonicVendorId = 0x066F;
        constexpr uint32_t MinasA6bProductCode = 0x60380005;
        constexpr uint32_t MinasA6bRevision = 0x00010000;

        /// <summary>Thứ tự slave mặc định: X, Y, Z (slave 0–2), rồi R1EC (slave 3). Thứ tự thật không quan trọng — sai thì
        /// sửa ở trang EtherCAT hoặc Scan rồi gán lại.</summary>
        constexpr int DefaultIoSlave = 3;

        /// <summary>Giá trị mặc định để app chạy được ngay lần đầu. <b>Phần lớn là PHỎNG ĐOÁN</b> —
        /// hành trình, tốc độ, Homing method, counts/đơn vị phải xác định lại trên máy thật (MINAS A6B: encoder
        /// 23 bit, 8388608 counts/vòng khi bánh răng điện tử 1:1).</summary>
        std::shared_ptr<AxisConfig> DefaultAxis(std::string_view name)
        {
            int slave = AxisId::IndexOf(name);
            auto cfg = std::make_shared<AxisConfig>();
            cfg->Name = std::string(name);
            cfg->SlavePosition = slave < 0 ? 0 : slave;
            cfg->AxisChannel = 0;
            cfg->CountsPerUnit = 1000.0;
            cfg->HomingMethod = 1;
            cfg->HomeDir = -1;
            cfg->AccelTimeS = 0.2;
            cfg->DecelTimeS = 0.2;
            cfg->ExpectedVendorId = PanasonicVendorId;
            cfg->ExpectedProductCode = MinasA6bProductCode;
            cfg->ExpectedRevision = MinasA6bRevision;
            cfg->ExpectedDriverName = "MINAS A6B";

            auto set = [&](const char* unit, double neg, double pos, double slow, double fast, double move, double home, double standby) {
                cfg->Unit = unit; cfg->SoftLimitNeg = neg; cfg->SoftLimitPos = pos;
                cfg->JogSpeedSlow = slow; cfg->JogSpeedFast = fast; cfg->MoveSpeed = move; cfg->HomingVelocity = home;
                cfg->StandbyPos = standby;
            };

            if (name == AxisId::X) set("mm", -1, 600, 20, 60, 300, 20, 10);
            else if (name == AxisId::Y) set("mm", -1, 400, 20, 60, 250, 20, 10);
            // Z: gốc ở phía TRÊN, dương = đi XUỐNG — vị trí chờ nằm dưới Z_SAFE.
            else if (name == AxisId::Z) set("mm", -1, 100, 10, 30, 150, 10, 5);
            return cfg;
        }

        json ToJson(const IoModuleConfig& c)
        {
            json j = json::object();
            j["Name"] = c.Name;
            j["SlavePosition"] = c.SlavePosition;
            j["ExpectedVendorId"] = c.ExpectedVendorId;
            j["ExpectedProductCode"] = c.ExpectedProductCode;
            j["ExpectedRevision"] = c.ExpectedRevision;
            j["ExpectedDriverName"] = c.ExpectedDriverName;
            return j;
        }

        void FromJson(const json& j, IoModuleConfig& c)
        {
            Get(j, "Name", c.Name);
            Get(j, "SlavePosition", c.SlavePosition);
            Get(j, "ExpectedVendorId", c.ExpectedVendorId);
            Get(j, "ExpectedProductCode", c.ExpectedProductCode);
            Get(j, "ExpectedRevision", c.ExpectedRevision);
            Get(j, "ExpectedDriverName", c.ExpectedDriverName);
        }

        json ToJson(const AxisConfig& c)
        {
            json j = json::object();
            if (!c.Name.empty()) j["Name"] = c.Name;
            j["Unit"] = c.Unit;
            j["SlavePosition"] = c.SlavePosition;
            j["AxisChannel"] = c.AxisChannel;
            j["CountsPerUnit"] = c.CountsPerUnit;
            j["HomingMethod"] = (int)c.HomingMethod;
            j["HomingVelocity"] = c.HomingVelocity;
            j["AccelTimeS"] = c.AccelTimeS;
            j["DecelTimeS"] = c.DecelTimeS;
            if (c.TravelPerRotation) j["TravelPerRotation"] = *c.TravelPerRotation;   // NullValueHandling.Ignore
            if (c.PulsesPerRotation) j["PulsesPerRotation"] = *c.PulsesPerRotation;
            j["WriteSoftLimitToDrive"] = c.WriteSoftLimitToDrive;
            j["ExpectedVendorId"] = c.ExpectedVendorId;
            j["ExpectedProductCode"] = c.ExpectedProductCode;
            j["ExpectedRevision"] = c.ExpectedRevision;
            j["ExpectedDriverName"] = c.ExpectedDriverName;
            j["SoftLimitPos"] = c.SoftLimitPos;
            j["SoftLimitNeg"] = c.SoftLimitNeg;
            j["InPositionTolerance"] = c.InPositionTolerance;
            j["JogSpeedSlow"] = c.JogSpeedSlow;
            j["JogSpeedFast"] = c.JogSpeedFast;
            j["MoveSpeed"] = c.MoveSpeed;
            j["MoveSpeedMid"] = c.MoveSpeedMid();
            j["MoveSpeedLow"] = c.MoveSpeedLow();
            j["HomeDir"] = c.HomeDir;
            j["HomeTimeoutMs"] = c.HomeTimeoutMs;
            j["MoveTimeoutMs"] = c.MoveTimeoutMs;
            j["HomeOffset"] = c.HomeOffset;
            j["StandbyPos"] = c.StandbyPos;
            j["WrapDegrees"] = c.WrapDegrees;
            return j;
        }

        void FromJson(const json& j, AxisConfig& c)
        {
            Get(j, "Name", c.Name);
            Get(j, "Unit", c.Unit);
            Get(j, "SlavePosition", c.SlavePosition);
            Get(j, "AxisChannel", c.AxisChannel);
            Get(j, "CountsPerUnit", c.CountsPerUnit);
            int homing = c.HomingMethod;
            Get(j, "HomingMethod", homing);
            c.HomingMethod = (int8_t)homing;
            Get(j, "HomingVelocity", c.HomingVelocity);
            Get(j, "AccelTimeS", c.AccelTimeS);
            Get(j, "DecelTimeS", c.DecelTimeS);
            Get(j, "TravelPerRotation", c.TravelPerRotation);
            Get(j, "PulsesPerRotation", c.PulsesPerRotation);
            Get(j, "WriteSoftLimitToDrive", c.WriteSoftLimitToDrive);
            Get(j, "ExpectedVendorId", c.ExpectedVendorId);
            Get(j, "ExpectedProductCode", c.ExpectedProductCode);
            Get(j, "ExpectedRevision", c.ExpectedRevision);
            Get(j, "ExpectedDriverName", c.ExpectedDriverName);
            Get(j, "SoftLimitPos", c.SoftLimitPos);
            Get(j, "SoftLimitNeg", c.SoftLimitNeg);
            Get(j, "InPositionTolerance", c.InPositionTolerance);
            Get(j, "JogSpeedSlow", c.JogSpeedSlow);
            Get(j, "JogSpeedFast", c.JogSpeedFast);
            Get(j, "MoveSpeed", c.MoveSpeed);
            double mid = 0, low = 0;
            Get(j, "MoveSpeedMid", mid);
            Get(j, "MoveSpeedLow", low);
            c.SetMoveSpeedMid(mid);
            c.SetMoveSpeedLow(low);
            Get(j, "HomeDir", c.HomeDir);
            Get(j, "HomeTimeoutMs", c.HomeTimeoutMs);
            Get(j, "MoveTimeoutMs", c.MoveTimeoutMs);
            Get(j, "HomeOffset", c.HomeOffset);
            Get(j, "StandbyPos", c.StandbyPos);
            Get(j, "WrapDegrees", c.WrapDegrees);
        }

#define SCS_VISION_FIELDS(X) \
    X(FlowLocateCan) X(OutFound) X(OutImage) X(RenderCam) X(GlobalVarModule) X(GvAxisX) X(GvAxisZ) X(GvNozzle) X(GvIsLocate)

#define SCS_VISION_CALIB_FIELDS(X) \
    X(CenterX) X(CenterZ) X(CenterSet) X(GridStepX) X(GridStepZ) X(CalibNozzle) X(VerifyStepMm) X(ShotsPerPoint) X(ResidualTolMm)

#define SCS_LASER_COMP_FIELDS(X) \
    X(GainX) X(GainZ) X(MaxCompMm) X(LaserPulseMinMs)

#define SCS_DEFINE_JSON(Type, FIELDS) \
        json ToJson(const Type& v) \
        { \
            json j = json::object(); \
            FIELDS(SCS_JSON_PUT) \
            return j; \
        } \
        void FromJson(const json& j, Type& v) \
        { \
            FIELDS(SCS_JSON_GET) \
        }
#define SCS_JSON_PUT(f) j[#f] = v.f;
#define SCS_JSON_GET(f) Get(j, #f, v.f);

        SCS_DEFINE_JSON(VisionOutputNames, SCS_VISION_FIELDS)
        SCS_DEFINE_JSON(VisionCalibSettings, SCS_VISION_CALIB_FIELDS)
        SCS_DEFINE_JSON(LaserCompSettings, SCS_LASER_COMP_FIELDS)

#undef SCS_JSON_PUT
#undef SCS_JSON_GET
#undef SCS_DEFINE_JSON
    }

    std::vector<double> VisionCalibSettings::ParseList(std::string_view text)
    {
        std::vector<double> out;
        std::string cur;
        auto flush = [&] {
            std::string t = base::Trim(cur);
            cur.clear();
            if (t.empty()) return;
            char* end = nullptr;
            double v = std::strtod(t.c_str(), &end);
            if (end && *end == '\0' && std::isfinite(v)) out.push_back(v);
        };
        for (char c : text)
        {
            if (c == ',' || c == ';') flush();
            else cur.push_back(c);
        }
        flush();
        return out;
    }

    std::string VisionOutputNames::RenderPath(std::string_view flow, std::string_view path)
    {
        std::string p = base::Trim(path);
        return p.empty() ? std::string(flow) : p;
    }

    std::shared_ptr<AxisConfig> SystemData::GetAxis(std::string_view name)
    {
        for (auto& a : Axes)
            if (a && a->Name == name) return a;
        auto cfg = DefaultAxis(name);
        Axes.push_back(cfg);
        return cfg;
    }

    std::shared_ptr<SystemData> SystemData::CreateDefault()
    {
        auto sys = std::make_shared<SystemData>();
        for (const char* name : AxisId::All)
            sys->Axes.push_back(DefaultAxis(name));
        sys->IoModule = DefaultIoModule();
        return sys;
    }

    std::shared_ptr<IoModuleConfig> SystemData::DefaultIoModule()
    {
        auto io = std::make_shared<IoModuleConfig>();
        io->Name = "R1EC";
        io->SlavePosition = DefaultIoSlave;
        io->ExpectedVendorId = LeadshineVendorId;
        io->ExpectedProductCode = R1ecProductCode;
        io->ExpectedRevision = R1ecRevision;
        io->ExpectedDriverName = "R1EC";
        return io;
    }

    std::string SystemData::ToJsonText() const
    {
        json j = json::object();
        j["MachineName"] = MachineName;
        j["MachineCode"] = MachineCode;
        j["SimulationMode"] = SimulationMode;
        j["VisionSimulation"] = VisionSimulation;
        j["NetworkAdapter"] = NetworkAdapter;
        j["RtReserveCore"] = RtReserveCore;
        j["RtCpuIndex"] = RtCpuIndex;
        j["RtCycleUs"] = RtCycleUs;
        j["RtSpinUs"] = RtSpinUs;
        if (IoModule) j["IoModule"] = ToJson(*IoModule);
        j["ZSafe"] = ZSafe;
        j["ZSafeInterlock"] = ZSafeInterlock;
        j["HasLightCurtain"] = HasLightCurtain;
        j["HasAirSensor"] = HasAirSensor;
        j["HasTraySensors"] = HasTraySensors;
        j["ControlTickMs"] = ControlTickMs;
        j["WatchdogTimeoutMs"] = WatchdogTimeoutMs;
        j["WatchdogStartupGraceMs"] = WatchdogStartupGraceMs;
        j["VisionSolutionPath"] = VisionSolutionPath;
        j["VisionTriggerTimeoutMs"] = VisionTriggerTimeoutMs;
        j["VisionSettleMs"] = VisionSettleMs;
        j["LampSettleMs"] = LampSettleMs;
        j["VisionOutputs"] = ToJson(VisionOutputs);
        j["VisionCalib"] = ToJson(VisionCalib);
        j["CalibValidDays"] = CalibValidDays;
        j["LaserComp"] = ToJson(LaserComp);
        json axes = json::array();
        for (auto& a : Axes)
            if (a) axes.push_back(ToJson(*a));
        j["Axes"] = std::move(axes);
        return jsonx::Dump(j);
    }

    std::shared_ptr<SystemData> SystemData::FromJsonText(const std::string& text)
    {
        json j;
        if (!jsonx::ParseObject(text, j)) return nullptr;

        auto s = std::make_shared<SystemData>();
        Get(j, "MachineName", s->MachineName);
        Get(j, "MachineCode", s->MachineCode);
        Get(j, "SimulationMode", s->SimulationMode);
        Get(j, "VisionSimulation", s->VisionSimulation);
        Get(j, "NetworkAdapter", s->NetworkAdapter);
        Get(j, "RtReserveCore", s->RtReserveCore);
        Get(j, "RtCpuIndex", s->RtCpuIndex);
        Get(j, "RtCycleUs", s->RtCycleUs);
        Get(j, "RtSpinUs", s->RtSpinUs);
        if (auto it = j.find("IoModule"); it != j.end())
        {
            if (it->is_object()) FromJson(*it, *s->IoModule);
            else if (it->is_null()) s->IoModule = nullptr;
        }
        Get(j, "ZSafe", s->ZSafe);
        Get(j, "ZSafeInterlock", s->ZSafeInterlock);
        Get(j, "HasLightCurtain", s->HasLightCurtain);
        Get(j, "HasAirSensor", s->HasAirSensor);
        Get(j, "HasTraySensors", s->HasTraySensors);
        Get(j, "ControlTickMs", s->ControlTickMs);
        Get(j, "WatchdogTimeoutMs", s->WatchdogTimeoutMs);
        Get(j, "WatchdogStartupGraceMs", s->WatchdogStartupGraceMs);
        Get(j, "VisionSolutionPath", s->VisionSolutionPath);
        Get(j, "VisionTriggerTimeoutMs", s->VisionTriggerTimeoutMs);
        Get(j, "VisionSettleMs", s->VisionSettleMs);
        Get(j, "LampSettleMs", s->LampSettleMs);
        if (auto it = j.find("VisionOutputs"); it != j.end() && it->is_object()) FromJson(*it, s->VisionOutputs);
        if (auto it = j.find("VisionCalib"); it != j.end() && it->is_object()) FromJson(*it, s->VisionCalib);
        Get(j, "CalibValidDays", s->CalibValidDays);
        if (auto it = j.find("LaserComp"); it != j.end() && it->is_object()) FromJson(*it, s->LaserComp);
        if (auto it = j.find("Axes"); it != j.end() && it->is_array())
        {
            for (const auto& e : *it)
            {
                if (!e.is_object()) continue;
                auto a = std::make_shared<AxisConfig>();
                FromJson(e, *a);
                s->Axes.push_back(a);
            }
        }
        return s;
    }
}
