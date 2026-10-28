// Kiểm thử tầng thiết bị chuyển động / I/O: FakeAxis, FakeIo, MotionConnect (mô phỏng) và đường code
// EtherCAT thật (EtherCatBusManager + EtherCatAxis + EtherCatIoDevice) chạy trên SimulatedEtherCatBus.
// Không mở card mạng nào; mọi dữ liệu ghi vào một thư mục tạm mới — không đụng D:\ShieldCanLaser.
#include "TestKit.h"

#include "Base/Async.h"
#include "Base/Text.h"
#include "Communication/AxisAngle.h"
#include "Communication/EtherCat/EtherCatAxis.h"
#include "Communication/EtherCat/EtherCatBusManager.h"
#include "Communication/EtherCat/EtherCatIoDevice.h"
#include "Communication/EtherCat/NativeBusFactory.h"
#include "Communication/MotionConnect.h"
#include "Communication/Sim/FakeAxis.h"
#include "Communication/Sim/FakeIo.h"
#include "Data/McDataManager.h"
#include "EtherCat/Soem/SoemEtherCatBus.h"
#include "EtherCat/Transport/SimulatedEtherCatBus.h"
#include "Infor/AlarmCatalog.h"
#include "Infor/AlarmManager.h"
#include "Infor/AxisId.h"
#include "Infor/IoMap.h"
#include "SysConfig/AppPaths.h"
#include "Utility/Log/LogWorker.h"

#include <cmath>
#include <filesystem>
#include <functional>
#include <thread>

using namespace scs;
namespace fs = std::filesystem;

namespace
{
    /// <summary>Vòng quét giả: gọi fn mỗi 5 ms trên luồng riêng (FakeAxis chỉ chạy khi có người gọi Refresh).</summary>
    struct Ticker
    {
        std::jthread thread;
        explicit Ticker(std::function<void()> fn)
            : thread([fn = std::move(fn)](std::stop_token st) { while (!st.stop_requested()) { fn(); base::Delay(5); } }) {}
    };

    int Code(const char* axis, AxisFault f) { return AlarmCode::ForAxis(axis, f); }

    std::shared_ptr<AxisConfig> MakeCfg(const char* name)
    {
        auto c = std::make_shared<AxisConfig>();
        c->Name = name; c->SoftLimitNeg = -1; c->SoftLimitPos = 600; c->JogSpeedSlow = 20; c->StandbyPos = 10;
        return c;
    }

    /// <summary>Bus mô phỏng báo thừa một slave trên mạng — nhánh "số slave lệch cấu hình".</summary>
    struct ExtraSlaveBus : ecm::SimulatedEtherCatBus
    {
        int SlaveCount() const override { return ecm::SimulatedEtherCatBus::SlaveCount() + 1; }
    };

    void FakeAxisTests()
    {
        AlarmManager::ForceClearAll();
        auto cfg = MakeCfg("X");
        auto ax = std::make_shared<FakeAxis>(cfg);
        Ticker tick([ax] { ax->Refresh(); });
        std::stop_source never;

        CHECK(ax->Name() == "X" && ax->Config() == cfg && !ax->IsConnected() && !ax->SetServoOn(true));
        CHECK_NEAR(ax->CurrentPos(), 10.0, 1e-9);
        CHECK(ax->Connect() && ax->IsConnected() && !ax->ServoOn());
        CHECK(!ax->Home(never.get_token()));                       // servo chưa bật
        CHECK(ax->SetServoOn(true) && ax->ServoOn());
        CHECK(!ax->MoveAbs(50, 100, never.get_token()) && AlarmManager::IsActive(Code("X", AxisFault::NotHomed)));

        FakeAxis::SetSpeedFactor(20);
        CHECK(ax->Home(never.get_token()) && ax->OrgOk() && !ax->Busy());
        CHECK_NEAR(ax->CurrentPos(), 0.0, 1e-9);
        CHECK(ax->MoveAbs(50, 300, never.get_token()) && !ax->Busy());
        CHECK_NEAR(ax->CurrentPos(), 50.0, 0.01);
        CHECK(ax->MoveInc(5, 300, never.get_token()));
        CHECK_NEAR(ax->CurrentPos(), 55.0, 0.02);
        CHECK(ax->GentleMoves() == 0 && ax->MoveAbs(40, 77, never.get_token(), true) && ax->GentleMoves() == 1 && ax->LastGentleSpeed() == 77);

        // Soft limit: đích ngoài hành trình bị từ chối, trục không nhúc nhích.
        CHECK(!ax->MoveAbs(700, 300, never.get_token()) && AlarmManager::IsActive(Code("X", AxisFault::SoftLimit)));
        CHECK(!ax->MoveInc(-100, 300, never.get_token()));
        CHECK_NEAR(ax->CurrentPos(), 40.0, 0.02);

        // Jog + Stop, Jog(0).
        FakeAxis::SetSpeedFactor(1);
        ax->Jog(+1, 50);
        base::Delay(120);
        CHECK(ax->Busy() && ax->CurrentPos() > 41.0);
        ax->Stop();
        double p1 = ax->CurrentPos();
        base::Delay(60);
        CHECK(!ax->Busy() && ax->CurrentPos() == p1);
        ax->Jog(-1, 50);
        base::Delay(60);
        ax->Jog(0, 0);
        CHECK(!ax->Busy() && ax->CurrentPos() < p1);

        // Huỷ giữa chừng qua stop_token.
        {
            std::stop_source cts;
            auto fut = base::RunAsync([&] { return ax->MoveAbs(500, 50, cts.get_token()); });
            base::Delay(100);
            CHECK(ax->Busy());
            base::Stopwatch sw;
            cts.request_stop();
            CHECK(!fut.get() && sw.ElapsedMilliseconds() < 1000 && !ax->Busy() && ax->CurrentPos() < 100);
        }

        // Timeout: SimStall chạy chậm 20 lần, MoveTimeoutMs ngắn.
        cfg->MoveTimeoutMs = 150;
        ax->SetSimStall(true);
        CHECK(!ax->MoveAbs(300, 50, never.get_token()) && AlarmManager::IsActive(Code("X", AxisFault::MoveTimeout)) && !ax->Busy());
        ax->SetSimStall(false);
        cfg->MoveTimeoutMs = 30000;
        cfg->HomeTimeoutMs = 100;
        ax->SetSimStall(true);
        CHECK(!ax->Home(never.get_token()) && AlarmManager::IsActive(Code("X", AxisFault::HomeTimeout)) && !ax->OrgOk());
        ax->SetSimStall(false);
        cfg->HomeTimeoutMs = 60000;
        FakeAxis::SetSpeedFactor(20);
        CHECK(ax->Home(never.get_token()) && ax->OrgOk());

        // Bơm alarm driver giữa lúc chạy rồi reset.
        FakeAxis::SetSpeedFactor(1);
        {
            auto fut = base::RunAsync([&] { return ax->MoveAbs(400, 50, never.get_token()); });
            base::Delay(60);
            ax->SetSimDriverAlarm(true);
            CHECK(!fut.get() && ax->Alarm() && ax->ErrorCode() == 0x1601 && AlarmManager::IsActive(Code("X", AxisFault::DriverAlarm)));
            CHECK(!ax->MoveAbs(10, 50, never.get_token()));
            ax->Jog(+1, 50);
            CHECK(!ax->Busy());
            CHECK(ax->ResetAlarm() && !ax->Alarm() && ax->ErrorCode() == 0);
        }

        // Cữ cứng suy từ vị trí; mất kết nối giả; Disconnect làm mất gốc.
        CHECK(!ax->FLS() && !ax->RLS());
        ax->SetSimDisconnected(true);
        CHECK(!ax->IsConnected() && !ax->SetServoOn(false));
        ax->SetSimDisconnected(false);
        CHECK(ax->SetServoOn(false) && !ax->ServoOn() && ax->OrgOk());   // tắt servo không mất gốc
        ax->Disconnect();
        CHECK(!ax->IsConnected() && !ax->ServoOn() && !ax->OrgOk());

        // Vị trí chờ trùng gốc → đặt lệch 15 theo chiều ngược HomeDir.
        auto c0 = MakeCfg("Y"); c0->StandbyPos = 0;
        CHECK_NEAR(FakeAxis(c0).CurrentPos(), 15.0, 1e-9);

        // Trục quay quấn vòng (máy này không có, lớp FakeAxis vẫn giữ): Move Abs đi đường ngắn, Move Inc nhích đúng delta.
        auto rc = MakeCfg("R"); rc->Unit = "deg"; rc->WrapDegrees = true; rc->SoftLimitNeg = -360; rc->SoftLimitPos = 360; rc->StandbyPos = 0;
        auto rot = std::make_shared<FakeAxis>(rc);
        Ticker tick2([rot] { rot->Refresh(); });
        FakeAxis::SetSpeedFactor(20);
        CHECK(rot->Connect() && rot->SetServoOn(true) && rot->Home(never.get_token()));
        CHECK_NEAR(rot->CurrentPos(), 0.0, 1e-9);
        FakeAxis::SetSpeedFactor(1);
        base::Stopwatch sw;
        CHECK(rot->MoveAbs(350, 100, never.get_token()));          // 0° → 350° = −10°, không phải +350°
        CHECK(sw.ElapsedMilliseconds() < 1500);
        CHECK(AxisAngle::Distance(rot->CurrentPos(), 350.0) < 0.01 && rot->CurrentPos() >= 0 && rot->CurrentPos() < 360);
        sw.Restart();
        CHECK(rot->MoveAbs(10, 100, never.get_token()) && sw.ElapsedMilliseconds() < 1500);   // 350° → 10° = +20°
        CHECK(AxisAngle::Distance(rot->CurrentPos(), 10.0) < 0.01);
        FakeAxis::SetSpeedFactor(50);
        CHECK(rot->MoveInc(200, 100, never.get_token()));           // nhích đúng +200°, không đổi thành −160°
        CHECK(AxisAngle::Distance(rot->CurrentPos(), 210.0) < 0.02);
        CHECK(rot->MoveAbs(370, 100, never.get_token()) && AxisAngle::Distance(rot->CurrentPos(), 10.0) < 0.01);   // không có soft limit
        CHECK(!rot->FLS() && !rot->RLS());
        FakeAxis::SetSpeedFactor(1);
        AlarmManager::ForceClearAll();
    }

    void FakeIoTests()
    {
        FakeIo io;
        int calls = 0, lastBit = -1;
        io.AfterSetDo = [&](int bit, bool) { ++calls; lastBit = bit; };
        CHECK(!io.IsConnected() && io.Connect() && io.IsConnected());
        CHECK(io.GetDi(IoMap::EmgOk) && io.GetDi(IoMap::LightCurtainOk) && io.GetDi(IoMap::AirOk) && !io.GetDi(IoMap::BtnStart));
        CHECK(io.GetDi(IoMap::TrayPresent(1)) && io.GetDi(IoMap::TrayPresent(2)) && !io.GetDi(IoMap::VacuumSensor(1)));
        CHECK(!io.GetDi(-1) && !io.GetDi(32) && !io.GetDo(-1) && !io.GetDo(32));
        io.SetDo(IoMap::DoBuzzer, true); io.SetDo(IoMap::DoBuzzer, true); io.SetDo(99, true);
        CHECK(io.GetDo(IoMap::DoBuzzer) && calls == 1 && lastBit == IoMap::DoBuzzer);
        io.SetDo(IoMap::DoBuzzer, false);
        CHECK(!io.GetDo(IoMap::DoBuzzer) && calls == 2);

        // Cảm biến chân không bám theo van của nozzle — trừ nozzle đặt hút trượt.
        io.SetDo(IoMap::DoVacuum(3), true);
        CHECK(io.GetDi(IoMap::VacuumSensor(3)) && !io.GetDi(IoMap::VacuumSensor(2)));
        io.SimSetPickFail(3, true);
        CHECK(!io.GetDi(IoMap::VacuumSensor(3)) && io.GetDo(IoMap::DoVacuum(3)));
        io.SetDo(IoMap::DoVacuum(3), false);
        io.SetDo(IoMap::DoVacuum(3), true);
        CHECK(!io.GetDi(IoMap::VacuumSensor(3)));
        io.SimSetPickFail(3, false);
        CHECK(io.GetDi(IoMap::VacuumSensor(3)));
        io.SetDo(IoMap::DoVacuum(3), false);
        CHECK(!io.GetDi(IoMap::VacuumSensor(3)));

        // Đếm xung laser: chỉ sườn lên.
        CHECK(io.LaserPulseCount() == 0);
        io.SetDo(IoMap::DoLaserTrigger, true); io.SetDo(IoMap::DoLaserTrigger, true); io.SetDo(IoMap::DoLaserTrigger, false);
        io.SetDo(IoMap::DoLaserTrigger, true); io.SetDo(IoMap::DoLaserTrigger, false);
        CHECK(io.LaserPulseCount() == 2 && !io.GetDo(IoMap::DoLaserTrigger));

        // DI giữ mức: sườn chỉ tồn tại tới vòng quét kế tiếp.
        io.SimSet(20, true);
        CHECK(io.GetDi(20) && io.RisingEdge(20));
        io.Refresh();
        CHECK(io.GetDi(20) && !io.RisingEdge(20) && !io.FallingEdge(20));
        io.SimSet(20, false);
        CHECK(io.FallingEdge(20));
        io.Refresh();
        CHECK(!io.FallingEdge(20));
        io.SimToggle(IoMap::EmgOk);
        CHECK(!io.GetDi(IoMap::EmgOk));

        // Nút nhấn-nhả: lên 1 đúng MỘT vòng quét rồi tự nhả.
        io.SimPress(IoMap::BtnStart);
        CHECK(!io.GetDi(IoMap::BtnStart));
        io.Refresh();
        CHECK(io.GetDi(IoMap::BtnStart) && io.RisingEdge(IoMap::BtnStart));
        io.Refresh();
        CHECK(!io.GetDi(IoMap::BtnStart) && io.FallingEdge(IoMap::BtnStart) && !io.RisingEdge(IoMap::BtnStart));
        io.Refresh();
        CHECK(!io.FallingEdge(IoMap::BtnStart));
        io.Disconnect();
        CHECK(!io.IsConnected());
    }

    void MotionSimTests()
    {
        AlarmManager::ForceClearAll();
        auto sys = McDataManager::SysData();
        sys->SimulationMode = true;
        MotionConnect motion;
        CHECK(!motion.AllConnected() && !motion.AnyAlarm() && motion.AxisList().empty());
        motion.Init(*sys);
        Ticker tick([&] { motion.Refresh(); });
        std::stop_source never;

        CHECK(motion.Simulation() && motion.Bus() == nullptr && motion.AxisList().size() == 3 && motion.Axes().size() == 3);
        CHECK(motion.Axis("Q") == nullptr && motion.Axis("Z") == motion.AxisList()[2] && motion.Axis("Z")->Config() == sys->GetAxis("Z"));
        CHECK(motion.Axis("Y") == motion.AxisList()[1] && std::dynamic_pointer_cast<FakeAxis>(motion.Axis("X")) != nullptr);
        CHECK(!motion.AllConnected() && motion.ConnectAll() && motion.AllConnected() && !motion.AllServoOn());
        for (auto& a : motion.AxisList()) CHECK(a->SetServoOn(true));
        CHECK(motion.AllServoOn() && !motion.AllHomed() && !motion.AnyAlarm() && !motion.AnyBusy());

        // Z_SAFE: Z chờ ở 5, ZSafe 10 (+1 dung sai) → an toàn. Hạ ngưỡng xuống 2 thì chặn, lý do nêu Z; tắt interlock thì luôn cho.
        CHECK(motion.ZSafe() == sys->ZSafe && motion.ZSafeLimit() == sys->ZSafe + 1.0 && motion.ZSafeInterlock());
        CHECK(motion.ZSafeFor("Z") == sys->ZSafe && motion.ZSafeLimitFor("Z") == sys->ZSafe + 1.0);
        CHECK_MSG(motion.ZSafeOk() && motion.ZUnsafeText().empty(), motion.ZUnsafeText());
        motion.SetZSafe(2.0);
        CHECK_MSG(!motion.ZSafeOk() && motion.ZUnsafeText() == "Z=5.00 (need ≤ 3.00)", motion.ZUnsafeText());
        motion.SetZSafeInterlock(false);
        CHECK(motion.ZSafeOk());
        motion.SetZSafeInterlock(true);
        motion.SetZSafe(sys->ZSafe);

        // Home qua MotionConnect: IsHoming bật trong lúc chạy, tắt khi xong.
        {
            auto z = motion.Axis(AxisId::Z);
            auto fut = base::RunAsync([&] { return motion.Home(z, never.get_token()); });
            base::Delay(80);
            CHECK(motion.IsHoming("Z") && !motion.IsHoming("X") && motion.AnyBusy());
            CHECK(fut.get() && !motion.IsHoming("Z") && z->OrgOk());
        }
        FakeAxis::SetSpeedFactor(50);
        for (auto& a : motion.AxisList())
            if (a->Name() != "Z") CHECK(motion.Home(a, never.get_token()));
        CHECK(motion.AllHomed());
        FakeAxis::SetSpeedFactor(1);

        motion.Axis("X")->Jog(+1, 20);
        motion.Axis("Y")->Jog(-1, 20);
        CHECK(motion.AnyBusy());
        motion.StopAll();
        CHECK(!motion.AnyBusy());

        std::dynamic_pointer_cast<FakeAxis>(motion.Axis("Y"))->SetSimDriverAlarm(true);
        base::Delay(40);
        CHECK(motion.AnyAlarm() && AlarmManager::IsActive(Code("Y", AxisFault::DriverAlarm)));
        CHECK(motion.Axis("Y")->ResetAlarm() && !motion.AnyAlarm());

        motion.DisconnectAll();
        base::Delay(40);
        CHECK(!motion.AllConnected() && !motion.AllHomed() && AlarmManager::IsActive(Code("X", AxisFault::Disconnected)));
        AlarmManager::ForceClearAll();
    }

    void EtherCatPathTests()
    {
        AlarmManager::ForceClearAll();
        auto sys = McDataManager::SysData();
        sys->SimulationMode = false;
        sys->NetworkAdapter = EtherCatBusManager::SimulatedAdapterSentinel;
        std::stop_source never;
        auto sysFn = [] { return McDataManager::SysData(); };

        MotionConnect motion;
        motion.Init(*sys);
        auto bus = motion.Bus();
        CHECK(!motion.Simulation() && bus != nullptr && bus->Service() != nullptr && !bus->IsConnected());
        CHECK(bus->BusDescription().find("SimulatedEtherCatBus") != std::string::npos && bus->ConnectionEpoch() == 0 && bus->LastFoundSlaveCount() == -1);
        // 3 driver A6B (slot 0, 1, 2) + Remote IO (slot 3) = 4 slave.
        CHECK(EtherCatBusManager::ExpectedSlaveCount(*sys) == 4 && EtherCatBusManager::DriverInfoDir() == AppPaths::ExeDir() / "DriverInfo");
        auto x = std::dynamic_pointer_cast<EtherCatAxis>(motion.Axis("X"));
        CHECK(x != nullptr && x->DiagnosticText().find("not on bus") != std::string::npos);
        auto sim = std::dynamic_pointer_cast<ecm::SimulatedEtherCatBus>(bus->Service()->Bus());
        CHECK(sim != nullptr);
        if (!x || !sim) return;

        CHECK(motion.ConnectAll() && bus->IsConnected() && motion.AllConnected());
        CHECK(bus->ConnectionEpoch() == 1 && bus->LastFoundSlaveCount() == 4 && bus->LastError().empty());
        CHECK(bus->LastAxisResults().size() == 3 && bus->LastIoResults().size() == 1 && bus->EnsureConnected());
        {
            auto z = std::dynamic_pointer_cast<EtherCatAxis>(motion.Axis("Z"));
            CHECK(z && z->IsConnected());
            CHECK_MSG(z && z->DiagnosticText().find(" S2 ") != std::string::npos, z ? z->DiagnosticText() : std::string());
        }
        CHECK(bus->RtStats().Cycles == 0 && bus->RealtimeInfo().empty() && bus->CyclicStats().SampleCount == 0);
        CHECK(!AlarmManager::IsActive(AlarmCode::EtherCatBusUnavailable) && !AlarmManager::IsActive(AlarmCode::EtherCatSlotInvalid));
        bool scanThrew = false;
        try { bus->ScanSlaves(); } catch (const std::runtime_error&) { scanThrew = true; }
        CHECK(scanThrew);

        // ── Trục: servo, về gốc, chạy, jog, soft limit ──
        CHECK(!x->ServoOn() && !x->Home(never.get_token()));
        CHECK(x->SetServoOn(true) && x->ServoOn() && !x->Alarm() && !x->OrgOk());
        CHECK(!x->MoveAbs(20, 200, never.get_token()) && AlarmManager::IsActive(Code("X", AxisFault::NotHomed)));
        CHECK(motion.Home(x, never.get_token()) && x->OrgOk() && !x->Busy() && !motion.IsHoming("X") && x->ServoOn());
        CHECK(x->MoveAbs(20, 200, never.get_token()) && !x->Busy());   // lệnh ĐẦU TIÊN sau về gốc phải chạy (bit4 đã hạ)
        CHECK_NEAR(x->CurrentPos(), 20.0, 0.011);
        CHECK(x->MoveInc(5, 200, never.get_token()));
        CHECK_NEAR(x->CurrentPos(), 25.0, 0.011);
        CHECK(x->MoveAbs(24, 5, never.get_token(), true));
        CHECK_NEAR(x->CurrentPos(), 24.0, 0.011);
        CHECK(!x->MoveAbs(700, 200, never.get_token()) && AlarmManager::IsActive(Code("X", AxisFault::SoftLimit)));
        CHECK(x->DiagnosticText().find("OpEn") != std::string::npos);

        x->Jog(+1, 50);
        base::Delay(200);
        x->Refresh();
        CHECK(x->Busy() && x->CurrentPos() > 25.0);
        x->Stop();
        base::Delay(300);
        x->Refresh();
        double p1 = x->CurrentPos();
        base::Delay(150);
        x->Refresh();
        CHECK(!x->Busy() && std::fabs(x->CurrentPos() - p1) < 0.01 && p1 < 300);
        x->Jog(-1, 50);
        base::Delay(150);
        x->Jog(0, 0);
        base::Delay(300);
        x->Refresh();
        CHECK(!x->Busy() && x->CurrentPos() < p1);
        CHECK(x->MoveAbs(10, 300, never.get_token()));                  // sau Halt vẫn nhận lệnh mới
        CHECK_NEAR(x->CurrentPos(), 10.0, 0.011);

        // Huỷ giữa chừng: trả false, trục dừng.
        {
            std::stop_source cts;
            auto fut = base::RunAsync([&] { return x->MoveAbs(500, 20, cts.get_token()); });
            base::Delay(150);
            cts.request_stop();
            CHECK(!fut.get() && !x->Busy());
            base::Delay(300);
            x->Refresh();
            CHECK(x->CurrentPos() < 100);
        }

        // Timeout chuyển động: trả false, alarm sự kiện +02, trục được ra lệnh dừng.
        x->Config()->MoveTimeoutMs = 120;
        CHECK(!x->MoveAbs(500, 1, never.get_token()) && AlarmManager::IsActive(Code("X", AxisFault::MoveTimeout)) && !x->Busy());
        x->Config()->MoveTimeoutMs = 30000;
        base::Delay(200);
        x->Refresh();
        CHECK(x->CurrentPos() < 100);

        // Trục Y, Z trên đường EtherCAT: về gốc và chạy tới đích.
        auto y = motion.Axis("Y");
        CHECK(y->SetServoOn(true) && motion.Home(y, never.get_token()) && y->MoveAbs(30, 200, never.get_token()));
        CHECK_NEAR(y->CurrentPos(), 30.0, 0.011);
        auto z = motion.Axis("Z");
        CHECK(z->SetServoOn(true) && motion.Home(z, never.get_token()) && z->MoveAbs(8, 100, never.get_token()));
        CHECK_NEAR(z->CurrentPos(), 8.0, 0.011);

        CHECK(x->SetServoOn(false));
        base::Delay(50);
        x->Refresh();
        CHECK(!x->ServoOn() && x->OrgOk() && !x->MoveAbs(5, 100, never.get_token()));
        CHECK(x->SetServoOn(true) && x->ResetAlarm());

        // ── Remote IO ──
        auto io = std::make_shared<EtherCatIoDevice>([] { return McDataManager::SysData()->IoModule; }, bus);
        const uint64_t nc = (1ULL << IoMap::EmgOk) | (1ULL << IoMap::LightCurtainOk);
        CHECK(sim->SimulateInputs()[sys->IoModule->SlavePosition] == nc);
        CHECK(io->Connect() && io->IsConnected());
        base::Delay(60);
        io->Refresh();
        CHECK(io->GetDi(IoMap::EmgOk) && io->GetDi(IoMap::LightCurtainOk) && !io->GetDi(IoMap::BtnStart));
        // Rack mô phỏng như máy thật: module 32 DI (X000…X031) rồi module 32 DO (Y000…Y031).
        CHECK(io->MappedDiCount() == 32 && io->MappedDoCount() == 32 && io->ModuleNames().size() == 2);
        CHECK(!io->GetDo(IoMap::DoBuzzer));
        io->SetDo(IoMap::DoBuzzer, true);
        io->SetDo(IoMap::DoCount, true);                                // ngoài IoMap: bỏ qua
        CHECK(io->GetDo(IoMap::DoBuzzer) && !io->GetDo(IoMap::DoCount) && !io->GetDo(IoMap::DoLampRed));
        CHECK(sim->GetSimulatedOutputs(sys->IoModule->SlavePosition) == (1ULL << IoMap::DoBuzzer));   // Y013 ở module DO
        io->SetDo(IoMap::DoBuzzer, false);
        CHECK(!io->GetDo(IoMap::DoBuzzer));
        sim->SetSimulateInputs(sys->IoModule->SlavePosition, nc | (1ULL << IoMap::BtnStart));
        base::Delay(60);
        io->Refresh();
        CHECK(io->GetDi(IoMap::BtnStart) && io->RisingEdge(IoMap::BtnStart));
        io->Refresh();
        CHECK(io->GetDi(IoMap::BtnStart) && !io->RisingEdge(IoMap::BtnStart));
        sim->SetSimulateInputs(sys->IoModule->SlavePosition, nc);
        base::Delay(60);
        io->Refresh();
        CHECK(io->FallingEdge(IoMap::BtnStart) && !AlarmManager::IsActive(AlarmCode::IoModuleFault));

        // ── Mở lại bus: lượt mới → mất cờ đã về gốc ──
        CHECK(x->OrgOk());
        CHECK(bus->Reconnect() && bus->ConnectionEpoch() == 2);
        x->Refresh();
        CHECK(x->IsConnected() && !x->OrgOk());

        // ── Slave không trả lời → 3101, trục báo mất kết nối kèm lý do ──
        AlarmManager::ForceClearAll();
        sim->SetSimulateMissingAt(2, true);
        CHECK(!bus->Reconnect() && !bus->IsConnected() && bus->LastFoundSlaveCount() == -1);
        CHECK(bus->LastError().find("NoResponse") != std::string::npos && AlarmManager::IsActive(AlarmCode::EtherCatSlotInvalid));
        CHECK(!motion.ConnectAll() && !motion.AllConnected() && AlarmManager::IsActive(Code("Z", AxisFault::Disconnected)));
        CHECK(AlarmManager::Get(Code("Z", AxisFault::Disconnected)).value_or(AlarmData{}).Detail.find("NoResponse") != std::string::npos);
        CHECK(!io->Connect() && !io->IsConnected() && bus->ConnectionEpoch() == 2);
        sim->SetSimulateMissingAt(2, false);

        // ── Chưa slot nào gán driver → 3101 ──
        AlarmManager::ForceClearAll();
        std::vector<uint32_t> saved;
        for (auto& a : sys->Axes) { saved.push_back(a->ExpectedProductCode); a->ExpectedProductCode = 0; }
        uint32_t savedIo = sys->IoModule->ExpectedProductCode;
        sys->IoModule->ExpectedProductCode = 0;
        CHECK(EtherCatBusManager::ExpectedSlaveCount(*sys) == 0 && !bus->Reconnect());
        CHECK(bus->LastError().find("No slot assigned a driver") != std::string::npos && AlarmManager::IsActive(AlarmCode::EtherCatSlotInvalid));
        for (size_t i = 0; i < sys->Axes.size(); ++i) sys->Axes[i]->ExpectedProductCode = saved[i];
        sys->IoModule->ExpectedProductCode = savedIo;
        // Một slot bỏ trống là hợp lệ: bỏ Z thì còn 3 slave, trục Z không nối, X/Y vẫn nối.
        sys->GetAxis("Z")->ExpectedProductCode = 0;
        CHECK(EtherCatBusManager::ExpectedSlaveCount(*sys) == 3);
        CHECK(bus->Reconnect() && bus->LastFoundSlaveCount() == 3 && bus->ConnectionEpoch() == 3);
        motion.Refresh();
        CHECK(!motion.Axis("Z")->IsConnected() && motion.Axis("X")->IsConnected() && motion.Axis("Y")->IsConnected());
        sys->GetAxis("Z")->ExpectedProductCode = saved[2];
        motion.DisconnectAll();
        CHECK(!bus->IsConnected());

        // ── Số slave trên mạng lệch cấu hình → 3101, bus bị đóng lại ──
        AlarmManager::ForceClearAll();
        {
            EtherCatBusManager extra(sysFn, std::make_shared<ExtraSlaveBus>(), "test");
            CHECK(!extra.EnsureConnected() && !extra.IsConnected() && extra.LastFoundSlaveCount() == 5 && extra.ConnectionEpoch() == 0);
            CHECK(extra.LastError().find("has 5 slaves, config requires 4") != std::string::npos && AlarmManager::IsActive(AlarmCode::EtherCatSlotInvalid));
            CHECK(!extra.EnsureConnected());                             // đã thử trong lượt này — không mở lại
        }

        // ── Chưa chọn card mạng → 3100; KHÔNG rơi về mô phỏng ──
        AlarmManager::ForceClearAll();
        sys->NetworkAdapter = "";
        {
            MotionConnect none;
            none.Init(*sys);
            auto b = none.Bus();
            CHECK(!none.Simulation() && b != nullptr && b->Service() == nullptr && b->BusDescription().rfind("BUS OPEN FAILED", 0) == 0);
            CHECK(!none.ConnectAll() && !none.AllConnected() && AlarmManager::IsActive(AlarmCode::EtherCatBusUnavailable));
            CHECK(b->LastError().find("No EtherCAT network adapter selected") != std::string::npos && b->ScanSlaves().empty());
            for (auto& a : none.AxisList())
                CHECK(std::dynamic_pointer_cast<FakeAxis>(a) == nullptr && !a->IsConnected() && !a->SetServoOn(true));
            EtherCatIoDevice io2([] { return McDataManager::SysData()->IoModule; }, b);
            CHECK(!io2.Connect() && !io2.GetDi(IoMap::EmgOk));
            io2.SetDo(0, true);
            none.StopAll();
            none.Refresh();
            CHECK(AlarmManager::IsActive(Code("X", AxisFault::Disconnected)));
            none.DisposeBus();
            CHECK(none.Bus() == nullptr);
        }
        CHECK(NativeBusFactory::IsAvailable() == ecm::SoemEtherCatBus::IsNpcapAvailable());

        motion.DisposeBus();
        sys->SimulationMode = true;
        AlarmManager::ForceClearAll();
    }
}

int RunMotionTests()
{
    testkit::Group g("Motion");

    fs::path root = fs::temp_directory_path() / ("scs_motion_" + base::NowString("yyyyMMdd_HHmmssfff"));
    AppPaths::Init(base::PathToU8(root));
    LogWorker::Instance().Init(AppPaths::Root());
    fs::path realAlarms = fs::path(__FILE__).parent_path().parent_path().parent_path() / "Config" / "alarms.json";
    if (!fs::exists(realAlarms)) realAlarms = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "Config" / "alarms.json";
    fs::path exeDir = root / "exe";
    fs::create_directories(exeDir / "Config");
    std::string tplText;
    CHECK_MSG(base::ReadAllText(realAlarms, tplText), base::PathToU8(realAlarms));
    base::WriteAllText(exeDir / "Config" / "alarms.json", tplText);
    AppPaths::SetExeDirForTests(exeDir);
    AlarmCatalog::Load();
    McDataManager::LoadSystemData();
    CHECK(McDataManager::SysData()->Axes.size() == 3 && AppPaths::Root() == root);

    FakeAxisTests();
    FakeIoTests();
    MotionSimTests();
    EtherCatPathTests();

    LogWorker::Instance().Flush();
    return g.Result();
}
