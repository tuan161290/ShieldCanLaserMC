// Kiểm thử tầng hạ tầng: AppPaths, LogWorker, AlarmCatalog/AlarmManager, SystemData/ModelData,
// AxisAngle, IoMap. Mọi dữ liệu ghi vào một thư mục tạm mới — không đụng D:\ShieldCanLaser.
#include "TestKit.h"

#include "Base/Async.h"
#include "Base/Text.h"
#include "Communication/AxisAngle.h"
#include "Data/McDataManager.h"
#include "Data/UserAccess.h"
#include "Infor/AlarmCatalog.h"
#include "Infor/AlarmManager.h"
#include "Infor/IoMap.h"
#include "SysConfig/AppPaths.h"
#include "Utility/Log/LogWorker.h"
#include "Utility/SingleInstanceGuard.h"
#include "Utility/UiThread.h"

#include <cmath>
#include <filesystem>

using namespace scs;
namespace fs = std::filesystem;

int RunInfraTests()
{
    testkit::Group g("Infra");

    fs::path root = fs::temp_directory_path() / ("scs_infra_" + base::NowString("yyyyMMdd_HHmmssfff"));
    AppPaths::Init(base::PathToU8(root));
    CHECK(AppPaths::Root() == root && !AppPaths::UsingFallbackRoot());
    CHECK(fs::is_directory(AppPaths::ImageNgDir()) && fs::is_directory(AppPaths::RuntimeDir()));
    CHECK(AppPaths::SystemFile() == root / "Config" / "system.json");
    LogWorker::Instance().Init(AppPaths::Root());

    // ── AxisAngle / IoMap / AlarmCode / AxisId ──
    CHECK_NEAR(AxisAngle::Wrap(-10), 350.0, 1e-9);
    CHECK_NEAR(AxisAngle::Wrap(370), 10.0, 1e-9);
    CHECK(AxisAngle::Wrap(360) == 0.0 && AxisAngle::Wrap(-1e-7) == 0.0 && AxisAngle::Wrap(std::nan("")) == 0.0);
    CHECK_NEAR(AxisAngle::ShortestDelta(350, 10), 20.0, 1e-9);
    CHECK_NEAR(AxisAngle::ShortestDelta(10, 350), -20.0, 1e-9);
    CHECK_NEAR(AxisAngle::Distance(10, 350), 20.0, 1e-9);
    CHECK(IoMap::BtnStart == 0 && IoMap::BtnStop == 1 && IoMap::EmgOk == 2 && IoMap::BtnReset == 3 && IoMap::LightCurtainOk == 4);
    CHECK(IoMap::TrayPresent(1) == 5 && IoMap::TrayPresent(2) == 6 && IoMap::NozzleCount == 7 && IoMap::TrayCount == 2);
    CHECK(IoMap::VacuumSensor(1) == 7 && IoMap::VacuumSensor(7) == 13 && IoMap::AirOk == 14);
    CHECK(IoMap::DoVacuum(1) == 3 && IoMap::DoVacuum(7) == 9 && IoMap::DoLampRed == 10 && IoMap::DoBuzzer == 13 && IoMap::DoCamLight == 14 && IoMap::DoLaserTrigger == 15);
    CHECK(IoMap::DiNames().size() == 32 && IoMap::DoNames().size() == 32 && IoMap::DiCount == 32 && IoMap::DoCount == 32);
    CHECK(IoMap::DiLabel(0) == "X000" && IoMap::DiLabel(31) == "X031" && IoMap::DoLabel(0) == "Y000" && IoMap::DoLabel(31) == "Y031");
    CHECK(IoMap::DiNames()[6] == "Tray 2 sensor" && IoMap::DiNames()[4] == "Photo safety sensor OK"
          && IoMap::DiNames()[13] == "Vacuum sensor N7" && IoMap::DiNames()[15] == "(spare)");
    CHECK(IoMap::DoNames()[13] == "Buzzer" && IoMap::DoNames()[9] == "Solenoid vacuum N7" && IoMap::DoNames()[15] == "Laser trigger" && IoMap::DoNames()[16] == "(spare)");
    CHECK(AlarmCode::ForAxis("Z", AxisFault::HomeTimeout) == 2205 && AlarmCode::ForAxis("Y", AxisFault::Disconnected) == 2100
          && AlarmCode::ForAxis("Q", AxisFault::HomeTimeout) == 1099);
    std::string ax; AxisFault af;
    CHECK(AlarmCode::TryParseAxis(2204, ax, af) && ax == "Z" && af == AxisFault::HardLimit && !AlarmCode::TryParseAxis(2300, ax, af) && !AlarmCode::TryParseAxis(1999, ax, af));
    CHECK(AxisId::All.size() == 3 && AxisId::IndexOf("Z") == 2 && AxisId::IndexOf("Y1") == -1 && AxisId::IsZ("Z") && !AxisId::IsZ("X"));
    CHECK(AxisId::PlanarAxes.size() == 2 && AxisId::ZAxes.size() == 1 && AxisId::GridRows == 1 && AxisId::GridColumns == 3);
    {
        int r = -1, c = -1;
        AxisId::GridSlot("Z", r, c);
        CHECK(r == 0 && c == 2);
    }

    // ── SystemData mặc định ──
    auto def = SystemData::CreateDefault();
    CHECK(def->Axes.size() == 3);
    for (size_t i = 0; i < def->Axes.size() && i < AxisId::All.size(); ++i)
        CHECK(def->Axes[i]->Name == AxisId::All[i] && def->Axes[i]->SlavePosition == (int)i && def->Axes[i]->AxisChannel == 0
              && def->Axes[i]->ExpectedDriverName == "MINAS A6B" && def->Axes[i]->ExpectedVendorId == 0x066Fu);
    CHECK(def->IoModule->SlavePosition == 3 && def->IoModule->ExpectedVendorId == 17185u && def->IoModule->ExpectedProductCode == 0x61400015u);
    CHECK(def->IoModule->DriverIdText() == "0x4321/0x61400015");
    auto x = def->GetAxis("X");
    CHECK(x->MoveSpeed == 300 && x->MoveSpeedMid() == 150 && x->MoveSpeedLow() == 60 && x->SoftLimitPos == 600 && x->ExpectedProductCode == 0x60380005u);
    CHECK(def->GetAxis("Y")->SoftLimitPos == 400 && def->GetAxis("Z")->StandbyPos == 5 && def->GetAxis("Z")->SoftLimitPos == 100);
    CHECK(def->SimulationMode && !def->VisionSimulation && def->ZSafe == 10.0 && def->ControlTickMs == 20 && def->HasTraySensors && !def->HasAirSensor);
    CHECK(def->VisionOutputs.FlowLocateCan == "LocateCan" && def->VisionOutputs.GvAxisX == "X_mm" && def->VisionOutputs.GvAxisZ == "Z_mm"
          && def->VisionOutputs.GvNozzle == "Nozzle" && def->VisionOutputs.GvIsLocate == "IsLocate");
    CHECK(def->LaserComp.GainX == -1 && def->LaserComp.GainZ == -1 && def->LaserComp.MaxCompMm == 3.0 && def->LaserComp.LaserPulseMinMs == 50);
    CHECK(def->VisionCalib.CalibNozzle == 1 && def->VisionCalib.GridStepZ == 5.0 && def->MachineName == "Shield CAN Laser Marking MC");

    // ── Lưu → nạp lại: so toàn bộ chuỗi JSON (mọi trường) ──
    auto sys = McDataManager::SysData();
    sys->MachineName = "Máy thử";
    sys->SimulationMode = false;
    sys->VisionSimulation = true;
    sys->NetworkAdapter = "\\Device\\NPF_{ABC}";
    sys->ZSafe = 12.5;
    sys->HasAirSensor = true;
    sys->VisionOutputs.RenderCam = "LocateCan.Module";
    sys->VisionOutputs.FlowLocateCan = "LC";
    sys->VisionCalib.CenterSet = true;
    sys->VisionCalib.CalibNozzle = 4;
    sys->LaserComp.GainX = 1.0;
    sys->LaserComp.MaxCompMm = 2.5;
    sys->GetAxis("Y")->TravelPerRotation = 5.0;
    sys->GetAxis("Y")->PulsesPerRotation = 10000.0;
    sys->GetAxis("Y")->RecomputeCountsPerUnit();
    CHECK(sys->GetAxis("Y")->CountsPerUnit == 2000.0);
    {
        // Thiếu một trong hai số (hoặc Travel = 0) thì không đụng CountsPerUnit gõ tay.
        AxisConfig manual;
        manual.CountsPerUnit = 123.0;
        manual.PulsesPerRotation = 10000.0;
        manual.RecomputeCountsPerUnit();
        manual.TravelPerRotation = 0.0;
        manual.RecomputeCountsPerUnit();
        CHECK(manual.CountsPerUnit == 123.0);
    }
    sys->GetAxis("Y")->SetMoveSpeedLow(33.0);
    std::string before = sys->ToJsonText();
    CHECK(McDataManager::SaveSystemData());
    McDataManager::LoadSystemData();
    auto re = McDataManager::SysData();
    CHECK(re != sys && re->ToJsonText() == before);
    CHECK(re->VisionSimulation && re->HasAirSensor && re->GetAxis("Y")->TravelPerRotation == 5.0 && re->GetAxis("Y")->MoveSpeedLow() == 33.0);
    CHECK(re->GetAxis("Y")->PulsesPerRotation == 10000.0 && re->GetAxis("Y")->CountsPerUnit == 2000.0 && !re->GetAxis("X")->PulsesPerRotation);
    CHECK(re->VisionOutputs.FlowLocateCan == "LC" && re->VisionCalib.CalibNozzle == 4 && re->LaserComp.GainX == 1.0 && re->LaserComp.MaxCompMm == 2.5);
    CHECK(before.find("IsConfigured") == std::string::npos && before.find("\"TravelPerRotation\": 5.0") != std::string::npos
          && before.find("\"LaserComp\"") != std::string::npos);

    // ── JSON dạng C#: thiếu khoá, khoá lạ, thiếu trục, trục lạ, sai thứ tự ──
    base::WriteAllText(AppPaths::SystemFile(), "\xEF\xBB\xBF{\r\n \"MachineName\": \"CS\", \"Unknown\": {\"a\": 1}, \"SimulationMode\": false,\r\n"
        " \"IoModule\": null, \"ControlTickMs\": 10,\r\n"
        " \"Axes\": [ {\"Name\": \"Z\", \"Unit\": \"mm\", \"HomingMethod\": -3, \"MoveSpeed\": 80.0, \"MoveSpeedMid\": 40.0, \"ExpectedVendorId\": 17185, \"Extra\": 1},\r\n"
        "   {\"Name\": \"BOGUS\"}, {\"Name\": \"X\", \"SlavePosition\": 4, \"TravelPerRotation\": null},\r\n"
        "   {\"Name\": \"Y\", \"ExpectedVendorId\": 17185, \"ExpectedProductCode\": 240, \"ExpectedDriverName\": \"L5N\"} ] }");
    McDataManager::LoadSystemData();
    auto cs = McDataManager::SysData();
    CHECK(cs->MachineName == "CS" && cs->MachineCode.empty() && !cs->SimulationMode && !cs->VisionSimulation && cs->ControlTickMs == 10 && cs->WatchdogTimeoutMs == 500);
    CHECK(cs->Axes.size() == 3 && cs->Axes[0]->Name == "X" && cs->Axes[0]->SlavePosition == 4 && cs->Axes[1]->Name == "Y" && cs->Axes[2]->Name == "Z");
    CHECK(cs->Axes[2]->HomingMethod == -3 && cs->Axes[2]->MoveSpeedMid() == 40.0 && cs->Axes[2]->MoveSpeedLow() == 16.0 && cs->Axes[2]->HomeTimeoutMs == 60000);
    CHECK(cs->IoModule && cs->IoModule->ExpectedRevision == 0x22092010u && cs->IoModule->SlavePosition == 3);
    // Mặc định cũ Leadshine L5N (0x4321/0xF0) → Panasonic MINAS A6B; mã khác (Z: chỉ có vendor, product 0) giữ nguyên.
    CHECK(cs->GetAxis("Y")->ExpectedVendorId == 0x066Fu && cs->GetAxis("Y")->ExpectedProductCode == 0x60380005u
          && cs->GetAxis("Y")->ExpectedDriverName == "MINAS A6B");
    CHECK(cs->GetAxis("Z")->ExpectedVendorId == 17185u);
    CHECK(!AlarmManager::IsActive(AlarmCode::ConfigInvalid));

    // ── ModelData ──
    McDataManager::LoadRecentModel();
    // Model mới: 8 điểm của chu trình Auto (chưa teach: 2 tray × 2 hàng + CAM N1/N7 + Laser N1/N7), tham số mặc định.
    CHECK(McDataManager::ModelLoaded() && McDataManager::Model()->Name == "DEFAULT" && McDataManager::Model()->TeachPositions.size() == 8);
    auto m = McDataManager::Model();
    CHECK(m->FindPosition(AutoPoints::PickRow(2, true)) && m->FindPosition(AutoPoints::Laser(false)) && !m->FindPosition(AutoPoints::Laser(false))->Taught());
    CHECK(m->Auto.TrayRows == 7 && m->Auto.VacuumOnMs == 150 && m->Auto.PickSlowLength == 2.0 && m->Auto.UseVision && m->Auto.UseLaser
          && m->Auto.MarkTimeMs == 700 && m->EnsureAutoPoints() == 0);
    CHECK(AutoPoints::PickRow(2, false) == "Pick Tray2 · Row 1" && AutoPoints::PickRow(1, true) == "Pick Tray1 · Last row"
          && AutoPoints::Cam(true) == "CAM · N7" && AutoPoints::Laser(false) == "Laser · N1");
    CHECK(AutoPoints::IsLayoutPoint("pick tray2 · last row") && AutoPoints::IsLayoutPoint(AutoPoints::Cam(false))
          && !AutoPoints::IsLayoutPoint("Standby") && AutoPoints::All().size() == 8);
    m->Auto.TrayRows = 3; m->Auto.VacuumOnMs = 20; m->Auto.UseVision = false; m->Auto.MarkTimeMs = 250;
    auto p = std::make_shared<TeachPosition>();   // điểm tự thêm ở tab Other
    p->Name = "Điểm thử";
    p->Note = "ghi chú";
    m->TeachPositions.push_back(p);
    CHECK(m->FindPosition("Điểm thử") == p && !p->Taught());
    p->Set("Z", 1.5); p->Set("Y", 90); p->Set("X", 2.25); p->Remove("Y");
    p->TaughtAt = std::chrono::system_clock::now();
    m->Description = "mô tả";
    m->Calib.Nozzle(3).RefX = 0.25; m->Calib.Nozzle(3).RefZ = -0.5; m->Calib.Nozzle(3).RefAt = std::chrono::system_clock::now();
    m->Calib.Nozzle(7).TrimX = 0.1;
    m->Calib.GridVerifiedAt = std::chrono::system_clock::now();
    CHECK(McDataManager::SaveModel("M1") && McDataManager::CopyModel("M1", "M2") && McDataManager::LoadModel("M2"));
    auto m2 = McDataManager::Model();
    auto p2 = m2->FindPosition("Điểm thử");
    CHECK(m2->Name == "M2" && m2->Description == "mô tả" && p2 && p2->Taught() && p2->Pos.size() == 2 && p2->Pos[0].first == "Z");
    CHECK(m2->Auto.TrayRows == 3 && m2->Auto.VacuumOnMs == 20 && !m2->Auto.UseVision && m2->Auto.UseLaser && m2->Auto.MarkTimeMs == 250);
    CHECK(m2->Calib.GridVerified() && m2->Calib.Nozzle(3).RefMeasured() && m2->Calib.Nozzle(3).RefX == 0.25 && m2->Calib.Nozzle(3).RefZ == -0.5
          && !m2->Calib.Nozzle(2).RefMeasured() && m2->Calib.Nozzle(7).TrimX == 0.1 && m2->Calib.Nozzle(0).RefX == 0.25 == false);
    CHECK(p2->Get("X") == 2.25 && p2->Get("Y", -1) == -1 && *p2->TaughtAt == *p->TaughtAt);
    CHECK(McDataManager::ListModels() == (std::vector<std::string>{ "DEFAULT", "M1", "M2" }) && McDataManager::RunData()->RecentModelName == "M2");
    CHECK(McDataManager::DeleteModel("M1") && McDataManager::ListModels().size() == 2);
    McDataManager::RunData()->RecordCycle(10.0, 4);
    McDataManager::RunData()->CountOk = 7;
    McDataManager::RunData()->TrayRowsDone[1] = 5;
    McDataManager::RunData()->LastRowResult = { 2, 3, 2, 2, 2, 2, 3 };
    McDataManager::RunData()->LastRowTray = 2;
    McDataManager::RunData()->LastRowIndex = 5;
    CHECK(McDataManager::SaveRunningData());
    auto savedAt = McDataManager::RunData()->SavedAt;
    McDataManager::LoadRunningData();
    auto run = McDataManager::RunData();
    CHECK(run->CountOk == 7 && run->RecentProductTimes.size() == 4 && run->AvgProductTimeSec == 2.5 && run->SavedAt == savedAt && run->RecentModelName == "M2");
    CHECK(run->TrayRowsDone[0] == 0 && run->TrayRowsDone[1] == 5 && run->LastRowResult.size() == 7 && run->LastRowResult[1] == 3 && run->LastRowTray == 2 && run->LastRowIndex == 5);
    run->ResetTrays();
    CHECK(run->TrayRowsDone[1] == 0 && run->LastRowResult.empty() && run->LastRowTray == 0);

    // ── Điểm chia đều giữa đầu và cuối: hàng tray, CAM, laser ──
    {
        auto g = ModelData::CreateDefault("G");
        std::string why;
        CHECK(!g->PickPoint(1, 1, why) && why.find(AutoPoints::PickRow(1, false)) != std::string::npos);
        auto teach = [&](const std::string& name, std::initializer_list<std::pair<const char*, double>> pos) {
            auto q = g->FindPosition(name);
            CHECK_MSG(q != nullptr, name);
            if (!q) return;
            for (const auto& [axis, v] : pos) q->Set(axis, v);
            q->TaughtAt = std::chrono::system_clock::now();
        };
        g->Auto.TrayRows = 4;
        teach(AutoPoints::PickRow(1, false), { { "X", 100 }, { "Y", 50 }, { "Z", 30 } });
        CHECK(!g->PickPoint(1, 2, why) && why.find(AutoPoints::PickRow(1, true)) != std::string::npos);
        teach(AutoPoints::PickRow(1, true), { { "X", 100.3 }, { "Y", 80 } });   // không có Z → giữ Z của hàng đầu; tray hơi xiên
        auto r1 = g->PickPoint(1, 1, why), r2 = g->PickPoint(1, 2, why), r4 = g->PickPoint(1, 4, why);
        CHECK(r1 && r1->Get("X") == 100 && r1->Get("Y") == 50 && r1->Get("Z") == 30 && r1->Name == "Pick Tray1 · R1");
        CHECK(r2 && std::fabs(r2->Get("X") - 100.1) < 1e-9 && std::fabs(r2->Get("Y") - 60) < 1e-9 && r2->Get("Z") == 30);
        CHECK(r4 && std::fabs(r4->Get("X") - 100.3) < 1e-9 && r4->Get("Y") == 80);
        CHECK(!g->PickPoint(1, 5, why) && !g->PickPoint(1, 0, why) && !g->PickPoint(3, 1, why) && !g->PickPoint(2, 1, why));
        g->Auto.TrayRows = 1;   // một hàng: không cần điểm cuối
        teach(AutoPoints::PickRow(2, false), { { "X", 10 }, { "Y", 200 }, { "Z", 5 } });
        auto one = g->PickPoint(2, 1, why);
        CHECK_MSG(one && one->Get("X") == 10 && one->Get("Y") == 200 && !g->PickPoint(2, 2, why), why);

        CHECK(!g->CamPoint(3, why) && why.find(AutoPoints::Cam(false)) != std::string::npos);
        teach(AutoPoints::Cam(false), { { "X", 300 }, { "Z", 20 } });
        teach(AutoPoints::Cam(true), { { "X", 360 }, { "Z", 21.2 } });
        auto c4 = g->CamPoint(4, why);
        CHECK(c4 && std::fabs(c4->Get("X") - 330) < 1e-9 && std::fabs(c4->Get("Z") - 20.6) < 1e-9 && c4->Name == "CAM · N4");
        CHECK(!g->CamPoint(0, why) && !g->CamPoint(8, why));
        teach(AutoPoints::Laser(false), { { "X", 400 }, { "Z", 25 } });
        teach(AutoPoints::Laser(true), { { "X", 460 }, { "Z", 25 } });
        auto l7 = g->LaserPoint(7, why);
        CHECK(l7 && l7->Get("X") == 460 && l7->Get("Z") == 25 && l7->Name == "Laser · N7");

        auto j = ModelData::FromJsonText(g->ToJsonText());
        CHECK(j && j->TeachPositions.size() == 8 && j->CamPoint(4, why) && std::fabs(j->CamPoint(4, why)->Get("X") - 330) < 1e-9 && j->EnsureAutoPoints() == 0);
    }

    // ── AlarmCatalog: bản mẫu thật cạnh "exe", hoà level + bổ sung mã thiếu ──
    fs::path realAlarms = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "Config" / "alarms.json";
    fs::path exeDir = root / "exe";
    fs::create_directories(exeDir / "Config");
    std::string tplText;
    CHECK_MSG(base::ReadAllText(realAlarms, tplText), base::PathToU8(realAlarms));
    base::WriteAllText(exeDir / "Config" / "alarms.json", tplText);
    AppPaths::SetExeDirForTests(exeDir);
    AlarmCatalog::Load();
    std::string copied;
    CHECK(AlarmCatalog::Loaded() && base::ReadAllText(AppPaths::AlarmsFile(), copied) && copied == tplText);
    for (int code : { 1000, 1001, 1002, 1003, 1099, 1100, 1103, 1105, 3000, 3001, 3002, 3100, 3101, 4000, 4001, 4002, 4003, 4004, 4005, 4006,
                      4010, 4011, 4090, 5001, 5002, 5003, 5004, 5005, 5006, 5010, 5020, 5030, 5031, 6000, 6001, 6002 })
        CHECK_MSG(AlarmCatalog::Get(code).Vi.find("chưa định nghĩa:") == std::string::npos && !AlarmCatalog::Get(code).Remedy.empty(), std::to_string(code));
    CHECK(AlarmCatalog::Get(1100).Vi == "DỪNG KHẨN CẤP (EMG)" && AlarmCatalog::Get(1100).Level == EAlarmLevel::Fatal);
    CHECK(AlarmCatalog::Get(2205).Vi == "Trục Z: về gốc thất bại" && AlarmCatalog::Get(2204).Level == EAlarmLevel::Warning);
    CHECK(AlarmCatalog::Get(5006).Level == EAlarmLevel::Warning && AlarmCatalog::Get(5004).Level == EAlarmLevel::Warning);
    CHECK(AlarmCatalog::Get(7777).Vi == "Alarm chưa định nghĩa: 7777" && AlarmCatalog::Get(7777).Level == EAlarmLevel::Error);
    base::WriteAllText(AppPaths::AlarmsFile(), "{ \"codes\": { \"1100\": { \"level\": 1, \"vi\": \"EMG của tôi\" }, \"9\": { \"level\": \"info\", \"vi\": \"riêng\" } } }");
    AlarmCatalog::Load();
    CHECK(AlarmCatalog::Get(1100).Vi == "EMG của tôi" && AlarmCatalog::Get(1100).Level == EAlarmLevel::Fatal);
    CHECK(AlarmCatalog::Get(9).Vi == "riêng" && AlarmCatalog::Get(6002).Level == EAlarmLevel::Warning && AlarmCatalog::Get(2000).En == "Axis X: communication lost");
    base::ReadAllText(AppPaths::AlarmsFile(), copied);
    CHECK(copied.find("\"level\": \"Fatal\"") != std::string::npos && copied.find("\"9\"") < copied.find("\"1000\""));

    // ── AlarmManager ──
    AlarmManager::ForceClearAll();
    int raisedEvents = 0;
    int token = AlarmManager::AlarmRaised().Subscribe([&](const AlarmData&) { ++raisedEvents; });
    uint64_t v0 = AlarmManager::Version();
    AlarmData a = AlarmManager::Raise(AlarmCode::AirPressureLow);
    CHECK(a.Source == "Safety" && a.Level == EAlarmLevel::Error && a.IsRootCause && a.ConditionActive && AlarmManager::Version() > v0);
    AlarmData b = AlarmManager::RaiseAxis("Z", AxisFault::HardLimit, "POT");
    CHECK(b.Code == 2204 && b.Source == "Z" && !b.IsRootCause && b.Display() == "[2204] Axis Z: hard limit hit — POT");
    uint64_t v1 = AlarmManager::Version();
    AlarmManager::Raise(AlarmCode::AirPressureLow);
    CHECK(raisedEvents == 2 && AlarmManager::ActiveCount() == 2 && AlarmManager::History().size() == 2 && AlarmManager::Version() == v1);
    AlarmManager::Raise(AlarmCode::AirPressureLow, {}, "0.3 MPa");
    CHECK(AlarmManager::Get(1103)->Detail == "0.3 MPa" && AlarmManager::Version() > v1);
    CHECK(AlarmManager::HasError() && !AlarmManager::HasFatal() && AlarmManager::HasWarning() && AlarmManager::RootCause()->Code == 1103);
    CHECK(!AlarmManager::PendingReset() && AlarmManager::Reset().size() == 2 && AlarmManager::ActiveCount() == 2);
    AlarmManager::ClearCondition(1103);
    CHECK(AlarmManager::IsActive(1103) && AlarmManager::PendingReset()->Code == 1103);
    auto stuck = AlarmManager::Reset();
    CHECK(stuck.size() == 1 && stuck[0].Code == 2204 && !AlarmManager::IsActive(1103) && AlarmManager::History()[0].ClearedAt.has_value());
    AlarmManager::Dismiss(2204);
    CHECK(AlarmManager::ActiveCount() == 0 && AlarmManager::History().size() == 2 && !AlarmManager::Get(2204));
    AlarmData e = AlarmManager::RaiseAxisEvent("X", AxisFault::HomeTimeout);
    CHECK(e.IsRootCause && !e.ConditionActive && AlarmManager::HasError() && AlarmManager::Reset().empty() && AlarmManager::ActiveAlarms().empty());
    AlarmManager::AlarmRaised().Unsubscribe(token);
    base::WaitBackgroundIdle(3000);
    AlarmManager::FlushCsv();
    std::string csv;
    fs::path day = AppPaths::LogDir() / base::NowString("yyyyMMdd");
    CHECK(base::ReadAllText(day / "alarm.csv", csv) && base::StartsWith(csv, "RaisedAt,ClearedAt,Code,Level,Source,RootCause,Message,Detail\r\n"));
    CHECK(csv.find(",1103,Error,Safety,ROOT,Air pressure low,\r\n") != std::string::npos && csv.find(",2204,Warning,Z,,Axis Z: hard limit hit,POT\r\n") != std::string::npos);

    // ── LogWorker ──
    uint64_t lv = LogWorker::Instance().Version();
    LogWorker::Instance().RecordLog("bỏ qua", LogLevel::Debug);
    LogWorker::Instance().RecordLog("Dòng\tthử\r\ntiếng Việt", LogLevel::Warning, "Test");
    LogWorker::Instance().Flush();
    auto list = LogWorker::Instance().LogList();
    CHECK(LogWorker::Instance().Version() == lv + 1 && !list.empty() && list[0].Message == "Dòng\tthử\r\ntiếng Việt" && list[0].Source == "Test");
    FILE* f = _wfopen((day / "app.log").c_str(), L"rb");
    std::string raw(1 << 20, '\0');
    raw.resize(f ? fread(raw.data(), 1, raw.size(), f) : 0);
    if (f) fclose(f);
    std::string expect = FormatLocal(list[0].UpdateTime, "yyyy-MM-dd HH:mm:ss.fff") + "\tWARNING\tTest\tDòng thử  tiếng Việt\r\n";
    CHECK(base::StartsWith(raw, "\xEF\xBB\xBF" "20") && base::EndsWith(raw, expect) && raw.find("bỏ qua") == std::string::npos);
    CHECK(raw.find("\tINFO\tData\tsystem.json loaded\r\n") != std::string::npos && raw.find("\tERROR\tSafety\tALARM 1103 [Error] Air pressure low  (root cause)\r\n") != std::string::npos);
    auto dayLines = LogWorker::ReadDay(AppPaths::Root(), std::chrono::system_clock::now());
    CHECK(!dayLines.empty() && dayLines.back().Level == LogLevel::Warning && dayLines.back().Message == "Dòng thử  tiếng Việt");
    CHECK(std::chrono::abs(dayLines.back().UpdateTime - list[0].UpdateTime) < std::chrono::milliseconds(2));

    // ── UiThread / SingleInstanceGuard ──
    int ran = 0;
    UiThread::Run([&] { ++ran; });
    UiThread::Post([&] { ++ran; });
    std::vector<std::function<void()>> posted;
    UiThread::SetDispatcher([&](std::function<void()> fn) { posted.push_back(std::move(fn)); });
    UiThread::Run([&] { ++ran; });
    UiThread::Post([&] { ++ran; });
    std::thread([&] { UiThread::Run([&] { ++ran; }); }).join();
    CHECK(ran == 3 && posted.size() == 2 && UiThread::IsUiThread());
    UiThread::SetDispatcher(nullptr);
    std::string mutexName = "Local\\scs_infra_test_" + base::NowString("HHmmssfff");
    CHECK(SingleInstanceGuard::TryLock(mutexName));
    SingleInstanceGuard::Release();

    // ── Phân quyền: Operator / Engineer, mật khẩu mặc định → đổi → lưu users.json, tự đăng xuất 10 phút ──
    {
        using UA = UserAccess;
        UA::Load();   // chưa có users.json → mật khẩu mặc định, cấp Operator
        CHECK(UA::Level() == EUserLevel::Operator && UA::UsingDefaultPassword() && UA::IdleLogoutMinutes() == 10);
        int changes = 0;
        EUserLevel last = EUserLevel::Operator;
        int token = UA::LevelChanged().Subscribe([&](EUserLevel l) { ++changes; last = l; });
        std::string why;
        CHECK(!UA::Login("wrong", why) && why == "Wrong password" && !UA::IsEngineer() && changes == 0);
        CHECK(UA::Login(UA::DefaultPassword, why) && UA::IsEngineer() && changes == 1 && last == EUserLevel::Engineer);
        CHECK(!UA::ChangePassword("bad", "abcd", why) && why == "Current password is wrong");
        CHECK(!UA::ChangePassword(UA::DefaultPassword, "abc", why) && why == "New password needs at least 4 characters");
        CHECK(UA::ChangePassword(UA::DefaultPassword, "laser2026", why) && !UA::UsingDefaultPassword());
        std::string usersText;
        CHECK(base::ReadAllText(AppPaths::UsersFile(), usersText) && usersText.find("laser2026") == std::string::npos
              && usersText.find("EngineerHash") != std::string::npos);   // chỉ lưu băm, không lưu chữ rõ

        // Tự đăng xuất: còn thao tác thì giữ, quá 10 phút không thao tác thì về Operator.
        auto t0 = UA::Clock::now();
        UA::Touch(t0);
        CHECK(!UA::CheckIdle(t0 + std::chrono::minutes(9)) && UA::IsEngineer());
        CHECK(UA::SecondsUntilIdleLogout(t0 + std::chrono::minutes(9)) == 60);
        UA::Touch(t0 + std::chrono::minutes(9));
        CHECK(!UA::CheckIdle(t0 + std::chrono::minutes(18)) && UA::IsEngineer());
        CHECK(UA::CheckIdle(t0 + std::chrono::minutes(19) + std::chrono::seconds(1)) && !UA::IsEngineer() && last == EUserLevel::Operator);
        CHECK(!UA::CheckIdle(t0 + std::chrono::hours(5)) && UA::SecondsUntilIdleLogout() == 0);   // Operator: không gì để đăng xuất
        CHECK(!UA::ChangePassword("laser2026", "x1234", why) && why == "Login as Engineer first");

        // Nạp lại: mật khẩu mới còn hiệu lực, mật khẩu mặc định hết hiệu lực; LOGOUT tay.
        UA::Load();
        CHECK(!UA::UsingDefaultPassword() && !UA::Login(UA::DefaultPassword, why));
        CHECK(UA::Login("laser2026", why) && UA::IsEngineer());
        UA::Logout("test");
        CHECK(!UA::IsEngineer() && last == EUserLevel::Operator);

        // Mật khẩu master: luôn đăng nhập được, đặt lại được mật khẩu Engineer đã quên.
        CHECK(UA::Login("22446688", why) && UA::IsEngineer());
        CHECK(UA::ChangePassword("22446688", "newpass", why) && UA::Login("newpass", why) && !UA::Login("laser2026", why));
        UA::Logout("test");
        CHECK(!UA::Login("2244668", why) && !UA::IsEngineer());
        UA::LevelChanged().Unsubscribe(token);
    }

    AppPaths::SetExeDirForTests({});
    LogWorker::Instance().Shutdown();
    std::error_code ec;
    fs::remove_all(root, ec);
    return g.Result();
}
