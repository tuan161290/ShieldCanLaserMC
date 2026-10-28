// Kiểm lớp điều khiển (McControl / McFunction / SafetyMonitor / routine / VmCycle) — chạy hoàn toàn mô phỏng.
#include "TestKit.h"

#include <algorithm>

#include "Base/Async.h"
#include "Base/Text.h"
#include "Communication/Sim/FakeAxis.h"
#include "Communication/Sim/FakeIo.h"
#include "Controller/McControl.h"
#include "Controller/VmCycle.h"
#include "Data/McDataManager.h"
#include "Infor/AlarmCatalog.h"
#include "Infor/AlarmManager.h"
#include "Infor/AxisId.h"
#include "Infor/IoMap.h"
#include "SysConfig/AppPaths.h"
#include "Utility/Log/LogWorker.h"

#include <cmath>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

using namespace scs;
namespace fs = std::filesystem;

namespace
{
    bool WaitFor(const std::function<bool()>& pred, int ms = 5000)
    {
        base::Stopwatch sw;
        while (!pred()) { if (sw.ElapsedMilliseconds() > ms) return false; base::Delay(5); }
        return true;
    }

    bool StartsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
    FakeIo& Io() { return *std::static_pointer_cast<FakeIo>(McControl::Io()); }
    McStatus& St() { return McControl::Status(); }
    std::shared_ptr<IAxis> Ax(const char* n) { return McControl::Motion().Axis(n); }
    void Tick() { base::Delay(80); }   // vài vòng quét

    /// <summary>Ghi vị trí X, Z ở mỗi sườn lên của DO kích laser — kiểm đích khắc đã bù (poll 1 ms, không móc vào FakeIo).</summary>
    struct LaserProbe
    {
        std::mutex m;
        std::vector<std::pair<double, double>> shots;
        std::jthread thread;
        LaserProbe() : thread([this](std::stop_token st) {
            bool prev = false;
            while (!st.stop_requested())
            {
                bool on = Io().GetDo(IoMap::DoLaserTrigger);
                if (on && !prev)
                {
                    std::lock_guard<std::mutex> l(m);
                    shots.emplace_back(Ax(AxisId::X)->CurrentPos(), Ax(AxisId::Z)->CurrentPos());
                }
                prev = on;
                base::Delay(1);
            }
        }) {}
        std::vector<std::pair<double, double>> Shots() { std::lock_guard<std::mutex> l(m); return shots; }
    };

    void RunOrigin()
    {
        McControl::RequestOrigin();
        CHECK(WaitFor([] { return St().State() == EMcState::Homing; }, 2000));
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && !McControl::RoutineBusy(); }, 20000));
    }

    void InitTests()
    {
        McControl::Init();
        CHECK(McControl::IsInitialized() && McControl::Io() && McControl::Vision() && McControl::Func() && McControl::Safety());
        CHECK(McControl::Vision()->IsSimulation() && McControl::Vision()->Fake());
        CHECK(WaitFor([] { return St().IoConnected() && St().MotionConnected() && St().VisionConnected(); }));
        CHECK(St().State() == EMcState::NotHomed && !St().OrgOk() && St().Simulation() && St().Mode() == EMcMode::Manual);
        CHECK(St().StateText() == "NOT HOMED");
        CHECK(WaitFor([] { return St().TowerLamp() == ETowerLamp::Yellow && Io().GetDo(IoMap::DoLampYellow) && Io().GetDo(IoMap::DoLampStopBtn); }));
        CHECK(St().EmgOk() && St().SafetyChainOk() && St().AirOk() && McControl::Safety()->SafeToMove() && McControl::Safety()->BlockReason().empty());
    }

    void ModeAndOriginTests()
    {
        std::mutex m; std::vector<std::string> refused;
        int token = McControl::AutoModeRefused().Subscribe([&](std::string s) { std::lock_guard<std::mutex> l(m); refused.push_back(s); });

        // AUTO lúc chưa về gốc → ở lại MANUAL, bắn sự kiện kèm lý do.
        McControl::RequestSetMode(EMcMode::Auto);
        CHECK(WaitFor([&] { std::lock_guard<std::mutex> l(m); return !refused.empty(); }));
        { std::lock_guard<std::mutex> l(m); CHECK_MSG(!refused.empty() && refused[0] == "Cannot enter AUTO — Not homed — press Origin first", refused.empty() ? "" : refused[0]); }
        Tick();
        CHECK(St().Mode() == EMcMode::Manual && St().State() == EMcState::NotHomed);

        // Về gốc bị chặn khi EMG.
        Io().SimSet(IoMap::EmgOk, false);
        CHECK(WaitFor([] { return St().State() == EMcState::Emg; }));
        McControl::RequestOrigin(); Tick();
        CHECK_MSG(St().Message() == "Cannot home: EMG active", St().Message());
        CHECK(!McControl::RoutineBusy());
        Io().SimSet(IoMap::EmgOk, true); Tick();
        CHECK(St().State() == EMcState::Emg);              // thoát EMG phải RESET
        Io().SimPress(IoMap::BtnReset);                    // nút RESET cứng (X003)
        CHECK(WaitFor([] { return St().State() == EMcState::NotHomed; }));
        CHECK(St().Message() == "RESET done");

        // Về gốc bị chặn khi trục báo alarm.
        auto x = std::static_pointer_cast<FakeAxis>(Ax(AxisId::X));
        x->SetSimDriverAlarm(true);
        CHECK(WaitFor([] { return !McControl::Safety()->SafeToMove(); }));
        McControl::RequestOrigin(); Tick();
        CHECK_MSG(StartsWith(St().Message(), "Cannot home: "), St().Message());
        x->SetSimDriverAlarm(false); Tick();
        McControl::RequestReset();
        CHECK(WaitFor([] { return St().State() == EMcState::NotHomed && McControl::Safety()->SafeToMove(); }));

        // Huỷ giữa chừng bằng nút STOP cứng (trục chậm lại để kịp bấm).
        FakeAxis::SetSpeedFactor(1);
        McControl::RequestOrigin();
        CHECK(WaitFor([] { return St().OriginStep() == EOriginState::O2_HomeZ || St().OriginStep() == EOriginState::O5_HomeXY; }));
        Io().SimPress(IoMap::BtnStop);
        CHECK(WaitFor([] { return St().State() == EMcState::NotHomed && !McControl::RoutineBusy(); }, 10000));
        CHECK(St().OriginStep() == EOriginState::Aborted && !St().OrgOk());
        CHECK_MSG(StartsWith(St().Message(), "Origin aborted at step O"), St().Message());
        FakeAxis::SetSpeedFactor(50);

        // Về gốc đầy đủ: Z trước, X/Y song song, về chờ.
        RunOrigin();
        CHECK(St().OrgOk() && St().OriginStep() == EOriginState::O7_Finish && McControl::Motion().AllHomed() && McControl::Motion().AllServoOn());
        CHECK_MSG(St().Message() == "Origin complete — machine ready", St().Message());
        bool atStandby = true;
        for (const auto& a : McControl::Motion().AxisList()) atStandby = atStandby && std::fabs(a->CurrentPos() - a->Config()->StandbyPos) < 0.05;
        CHECK(atStandby && McControl::Motion().ZSafeOk());

        // UnsafePause chặn AUTO.
        McDataManager::RunData()->UnsafePause = true;
        McControl::RequestSetMode(EMcMode::Auto);
        CHECK(WaitFor([&] { std::lock_guard<std::mutex> l(m); return refused.size() >= 2; }));
        { std::lock_guard<std::mutex> l(m); CHECK(refused.size() >= 2 && StartsWith(refused[1], "Cannot enter AUTO — Last stop was mid-motion")); }
        McControl::RequestInit();
        CHECK(WaitFor([] { return St().Message() == "Init done — machine at standby" && St().State() == EMcState::Idle; }, 10000));
        CHECK(!McDataManager::RunData()->UnsafePause);

        // AUTO → Armed → START vật lý → model chưa teach điểm của chu trình → dừng (ĐÃ DỪNG) kèm tên điểm.
        McControl::RequestSetMode(EMcMode::Auto);
        CHECK(WaitFor([] { return St().State() == EMcState::Armed && St().Mode() == EMcMode::Auto; }));
        CHECK(WaitFor([] { return St().TowerLamp() == ETowerLamp::GreenBlink; }));
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }));
        CHECK_MSG(St().Message() == "Cycle stopped: point \"Pick Tray1 · Row 1\" not taught (Teaching page)", St().Message());
        CHECK(McControl::CurrentCycleSec() >= 0 && McControl::CurrentCycleSec() < 1.0);
        McControl::RequestStart(); Tick();
        CHECK_MSG(St().Message() == "Cannot run in state STOPPED", St().Message());
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        McControl::RequestStop();
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && St().Message() == "Exited ready state"; }));
        McControl::RequestSetMode(EMcMode::Manual);
        CHECK(WaitFor([] { return St().Mode() == EMcMode::Manual; }));
        McControl::RequestArm(); Tick();
        CHECK(St().Message() == "Select AUTO mode first" && St().State() == EMcState::Idle);
        McControl::RequestStart(); Tick();
        CHECK(St().Message() == "Select AUTO mode before pressing START");
        McControl::AutoModeRefused().Unsubscribe(token);
    }

    /// Toạ độ mô phỏng cho mọi điểm teach của chu trình (trong soft limit mặc định), tray 2 hàng cho nhanh:
    /// hàng r của tray t ở X 100, Y = 50 + 150(t − 1) + 20(r − 1), Z 30. CAM N_k ở X = 290 + 10k, Z 20. Laser N_k ở X = 390 + 10k, Z 25.
    /// Master của 7 nozzle đã đặt (Ref = 0 — can nằm đúng giữa nozzle), lưới đã kiểm.
    void TeachAutoPoints()
    {
        auto model = McDataManager::Model();
        model->EnsureAutoPoints();
        auto set = [&](const std::string& name, std::initializer_list<std::pair<const char*, double>> pos) {
            auto p = model->FindPosition(name);
            CHECK_MSG(p != nullptr, name);
            if (!p) return;
            p->Pos.clear();
            for (const auto& [axis, v] : pos) p->Set(axis, v);
            p->TaughtAt = std::chrono::system_clock::now();
        };
        for (int t = 1; t <= 2; ++t)
        {
            double y = 50.0 + 150 * (t - 1);
            set(AutoPoints::PickRow(t, false), { { "X", 100 }, { "Y", y }, { "Z", 30 } });
            set(AutoPoints::PickRow(t, true), { { "X", 100 }, { "Y", y + 20 }, { "Z", 30 } });
        }
        set(AutoPoints::Cam(false), { { "X", 300 }, { "Z", 20 } });
        set(AutoPoints::Cam(true), { { "X", 360 }, { "Z", 20 } });
        set(AutoPoints::Laser(false), { { "X", 400 }, { "Z", 25 } });
        set(AutoPoints::Laser(true), { { "X", 460 }, { "Z", 25 } });
        model->Auto.TrayRows = 2;
        model->Auto.VacuumOnMs = 20;
        model->Auto.VacuumOffMs = 20;
        model->Auto.PickSlowLength = 2;
        model->Auto.MarkTimeMs = 60;
        model->Auto.UseVision = true;
        model->Auto.UseLaser = true;
        model->Calib = VisionCalib{};
        model->Calib.GridVerifiedAt = std::chrono::system_clock::now();
        for (int k = 1; k <= VisionCalib::NozzleCount; ++k) model->Calib.Nozzle(k).RefAt = std::chrono::system_clock::now();
    }

    /// Chu trình Laser Marking đủ vòng trên trục/IO/vision mô phỏng: gắp cả hàng, soi từng nozzle, khắc đã bù X/Z, trả về
    /// tray; hút hụt → ô NG; độ lệch vượt ngưỡng → ô NG không khắc; vision không thấy → ô NG; hết tray → chờ START thay
    /// tray; START không có tray → từ chối; STOP sau khi gắp → trả hàng về rồi mới dừng.
    void AutoCycleTests()
    {
        auto r = McControl::AutoRoutine();
        auto run = McDataManager::RunData();
        auto fake = McControl::Vision()->Fake();
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && St().OrgOk() && !McControl::RoutineBusy(); }));
        TeachAutoPoints();
        run->ResetTrays();
        run->CountOk = run->CountNg = run->CountTotal = 0;
        McDataManager::SysData()->LampSettleMs = 0;
        McDataManager::SysData()->VisionSettleMs = 0;
        fake->SetProcessTimeMs(2);
        fake->SetFoundRate(1);
        for (int k = 1; k <= 7; ++k) fake->SetNozzleDeviation(k, 0, 0);
        fake->SetNozzleDeviation(3, 0.4, -0.2);
        Io().SimSetPickFail(5, true);
        Io().SimSet(IoMap::TrayPresent(1), true);
        Io().SimSet(IoMap::TrayPresent(2), true);
        const int pulses0 = Io().LaserPulseCount();
        const int locate0 = fake->LocateCount();

        McControl::RequestSetMode(EMcMode::Auto);
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        CHECK(WaitFor([] { return Io().GetDo(IoMap::DoLampStartBtn); }));   // nhấp nháy

        // Van hút còn mở từ lần trước → không chạy, báo rõ nozzle.
        Io().SetDo(IoMap::DoVacuum(2), true);
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }));
        CHECK_MSG(StartsWith(St().Message(), "Cycle stopped: Nozzle 2 vacuum still ON"), St().Message());
        Io().SetDo(IoMap::DoVacuum(2), false);
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));

        // ── Lượt 1: 2 tray × 2 hàng, nozzle 5 hút hụt, can trên nozzle 3 lệch (0.4, −0.2) ──
        {
            LaserProbe probe;
            Io().SimPress(IoMap::BtnStart);
            CHECK(WaitFor([&] { return r->WaitingTray && St().State() == EMcState::WaitOperator && St().AutoStep() == EAutoState::S60_WaitTray; }, 60000));
            CHECK_MSG(run->TrayRowsDone[0] == 2 && run->TrayRowsDone[1] == 2 && run->CountOk == 24 && run->CountNg == 4 && run->CountTotal == 28, St().Message());
            CHECK(Io().LaserPulseCount() == pulses0 + 24 && fake->LocateCount() == locate0 + 24);
            CHECK_MSG(St().Message().find("Trays done — replace trays, then press START") != std::string::npos
                      && StartsWith(St().Message(), "Tray 2 row 2 done: 6 OK, 1 NG"), St().Message());
            CHECK(run->LastRowTray == 2 && run->LastRowIndex == 2 && run->LastRowResult.size() == 7 && run->LastRowResult[4] == (int)ECellState::Ng
                  && run->LastRowResult[0] == (int)ECellState::Ok && run->RecentProductTimes.size() == RunningData::CycleAvgWindow);   // 28 can, cửa sổ 25
            CHECK(std::fabs(Ax(AxisId::X)->CurrentPos() - 10) < 0.05 && std::fabs(Ax(AxisId::Y)->CurrentPos() - 10) < 0.05 && McControl::Motion().ZSafeOk());
            for (int k = 1; k <= 7; ++k) CHECK(!Io().GetDo(IoMap::DoVacuum(k)));
            CHECK(!Io().GetDo(IoMap::DoCamLight) && !Io().GetDo(IoMap::DoLaserTrigger));
            CHECK(WaitFor([] { return Io().GetDo(IoMap::DoLampStopBtn) && Io().GetDo(IoMap::DoLampStartBtn); }));   // chờ START: STOP sáng, START nháy
            CHECK(AlarmManager::IsActive(AlarmCode::PickFailed) && AlarmManager::IsActive(AlarmCode::TrayEmpty) && !AlarmManager::HasError());

            // Đích khắc: N1 (400, 25); N3 bù Gain −1 → (419.6, 25.2); N4 (430, 25); N5 không khắc.
            auto shots = probe.Shots();
            CHECK_MSG(shots.size() == 24, std::to_string(shots.size()));
            if (shots.size() == 24)
            {
                CHECK_NEAR(shots[0].first, 400.0, 0.03); CHECK_NEAR(shots[0].second, 25.0, 0.03);
                CHECK_NEAR(shots[2].first, 419.6, 0.03); CHECK_NEAR(shots[2].second, 25.2, 0.03);
                CHECK_NEAR(shots[3].first, 430.0, 0.03); CHECK_NEAR(shots[5].first, 460.0, 0.03);
                CHECK_NEAR(shots[6].first, 400.0, 0.03);   // hàng 2 lại từ N1
            }

            auto view = Routine_Auto::Trays();
            CHECK(view.Trays[0].size() == 14 && view.Trays[1].size() == 14);
            for (int t = 0; t < 2; ++t)
                for (int i = 0; i < 14; ++i)
                    CHECK_MSG(view.Trays[t][i] == (i % 7 == 4 ? (int)ECellState::Ng : (int)ECellState::Ok), std::format("tray {} cell {}", t, i));
            auto noz = Routine_Auto::Nozzles();
            CHECK(noz[3].HasDeviation && std::fabs(noz[3].Dx - 0.4) < 0.02 && std::fabs(noz[3].Dz + 0.2) < 0.02 && noz[3].State == (int)ECellState::Empty);
            CHECK(noz[5].State == (int)ECellState::Empty && noz[5].Text == "pick missed" && noz[1].Text == "returned (OK)");
            CHECK_MSG(Routine_Auto::ProgressText().find("Tray 1: 2/2 rows · Tray 2: 2/2 rows done") != std::string::npos
                      && Routine_Auto::ProgressText().find("N5·NG") != std::string::npos, Routine_Auto::ProgressText());
        }

        // ── START lúc chờ mà không có tray → từ chối, vẫn chờ ──
        Io().SimSet(IoMap::TrayPresent(1), false);
        Io().SimSet(IoMap::TrayPresent(2), false);
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().Message() == "START rejected: no tray detected (X005/X006)"; }));
        CHECK(r->WaitingTray && St().State() == EMcState::WaitOperator);

        // ── Lượt 2: chỉ tray 1, nozzle 3 lệch 3.5 mm > ngưỡng 3 → ô NG không khắc; nozzle 5 hút lại được ──
        Io().SimSetPickFail(5, false);
        fake->SetNozzleDeviation(3, 3.5, 0);
        Io().SimSet(IoMap::TrayPresent(1), true);
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([&] { return !r->WaitingTray && St().State() == EMcState::Running; }));   // đã nhận tray mới, chạy lại từ hàng 1
        CHECK(run->TrayRowsDone[0] == 0 || St().State() == EMcState::Running);
        CHECK(WaitFor([&] { return r->WaitingTray && St().State() == EMcState::WaitOperator; }, 60000));
        CHECK_MSG(run->TrayRowsDone[0] == 2 && run->TrayRowsDone[1] == 0 && run->CountOk == 36 && run->CountNg == 6, St().Message());
        CHECK(Io().LaserPulseCount() == pulses0 + 36 && fake->LocateCount() == locate0 + 38);
        CHECK(AlarmManager::IsActive(AlarmCode::CompensationOutOfRange));
        {
            auto view = Routine_Auto::Trays();
            CHECK(view.Trays[1].size() == 14 && view.Trays[1][0] == (int)ECellState::Empty && view.Trays[0][2] == (int)ECellState::Ng
                  && view.Trays[0][4] == (int)ECellState::Ok && view.Trays[0][9] == (int)ECellState::Ng);
            auto noz = Routine_Auto::Nozzles();
            CHECK_MSG(noz[3].State == (int)ECellState::Empty && noz[3].Text == "returned (NG)" && std::fabs(noz[3].Dx - 3.5) < 0.02, noz[3].Text);
        }
        fake->SetNozzleDeviation(3, 0, 0);

        // ── STOP lúc chờ thay tray → thoát chu trình, máy ĐÃ DỪNG ──
        McControl::RequestStop();
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }));
        CHECK(!r->WaitingTray);
        CHECK(WaitFor([] { return !Io().GetDo(IoMap::DoLampStartBtn); }));

        // ── STOP ngay sau khi gắp (lúc soi ảnh đầu): trả cả hàng về tray rồi mới dừng — hàng chưa tính, không khắc ──
        Io().SimSet(IoMap::TrayPresent(2), true);
        fake->SetFoundRate(0);   // vision không thấy
        std::atomic<int> flows{ 0 };
        fake->SetAfterFlow([&](const std::string&) { if (flows++ == 0) McControl::RequestPause(); });
        // Cảnh báo của lượt trước (5001/5004/5020) phải RESET xác nhận rồi mới vào sẵn sàng lại.
        McControl::RequestReset();
        CHECK(WaitFor([] { return AlarmManager::ActiveCount() == 0 && St().Message() == "RESET done"; }));
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }, 30000));
        fake->SetAfterFlow({});
        CHECK(run->TrayRowsDone[0] == 2 && run->TrayRowsDone[1] == 0 && run->CountNg == 6 && Io().LaserPulseCount() == pulses0 + 36);
        for (int k = 1; k <= 7; ++k) CHECK(!Io().GetDo(IoMap::DoVacuum(k)) && !Io().GetDi(IoMap::VacuumSensor(k)));
        CHECK(McControl::Motion().ZSafeOk());
        {
            auto view = Routine_Auto::Trays();
            CHECK(view.Trays[1].size() == 14 && view.Trays[1][0] == (int)ECellState::Loaded && view.Trays[1][6] == (int)ECellState::Loaded);
        }

        // ── Chạy tiếp: tray 2 hai hàng, vision không thấy → 14 ô NG, không khắc ──
        McControl::RequestReset();
        CHECK(WaitFor([] { return AlarmManager::ActiveCount() == 0; }));
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([&] { return r->WaitingTray && St().State() == EMcState::WaitOperator; }, 60000));
        CHECK_MSG(run->TrayRowsDone[1] == 2 && run->CountOk == 36 && run->CountNg == 20 && Io().LaserPulseCount() == pulses0 + 36, St().Message());
        CHECK(AlarmManager::IsActive(AlarmCode::VisionNg) && StartsWith(St().Message(), "Tray 2 row 2 done: 0 OK, 7 NG"));
        {
            auto noz = Routine_Auto::Nozzles();
            CHECK_MSG(!noz[1].HasDeviation && noz[1].Text == "returned (NG)", noz[1].Text);
        }
        fake->SetFoundRate(1);

        // ── Tắt vision + tắt laser: chỉ gắp, tới điểm khắc (chỉ Trim), trả về — không soi, không kích ──
        McDataManager::Model()->Auto.UseVision = false;
        McDataManager::Model()->Auto.UseLaser = false;
        Io().SimSet(IoMap::TrayPresent(2), false);
        const int locate1 = fake->LocateCount();
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([&] { return !r->WaitingTray; }));
        CHECK(WaitFor([&] { return r->WaitingTray && St().State() == EMcState::WaitOperator; }, 60000));
        CHECK_MSG(run->TrayRowsDone[0] == 2 && run->CountOk == 50 && run->CountNg == 20 && Io().LaserPulseCount() == pulses0 + 36
                  && fake->LocateCount() == locate1, St().Message());
        McDataManager::Model()->Auto.UseVision = true;
        McDataManager::Model()->Auto.UseLaser = true;

        // ── RESET TRAY từ trang Main lúc đang chờ = START (đã thay tray) ──
        Io().SimSet(IoMap::TrayPresent(2), true);
        CHECK(McControl::ResetTrays().empty());
        CHECK(WaitFor([&] { return !r->WaitingTray && St().State() == EMcState::Running; }));
        // STOP giữa lúc chạy = giữ tại chỗ (bit RUN tắt), RESET TRAY bị từ chối, START chạy tiếp; dừng hẳn ở checkpoint.
        McControl::RequestStop();
        CHECK(WaitFor([] { return St().State() == EMcState::Held && !McControl::Func()->RunBit(); }));
        CHECK(McControl::ResetTrays() == "cycle in progress — stop first");
        McControl::RequestStart();
        CHECK(WaitFor([] { return St().State() == EMcState::Running && McControl::Func()->RunBit(); }));
        McControl::RequestPause();
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }, 30000));
        for (int k = 1; k <= 7; ++k) CHECK(!Io().GetDo(IoMap::DoVacuum(k)));   // không bao giờ dừng với can trên nozzle
        CHECK(McControl::ResetTrays().empty() && run->TrayRowsDone[0] == 0 && run->TrayRowsDone[1] == 0 && run->LastRowResult.empty());
        CHECK(St().Message() == "Tray progress reset — next cycle starts from row 1 of tray 1");

        McControl::RequestSetMode(EMcMode::Manual);
        CHECK(WaitFor([] { return St().Mode() == EMcMode::Manual; }));
        McControl::RequestReset();
        CHECK(WaitFor([] { return AlarmManager::ActiveCount() == 0; }));
        Io().SimSet(IoMap::TrayPresent(1), true);
    }

    /// <summary>
    /// Các bước hiệu chuẩn của trang Vision Edit (Routine_Calib) trên trục/IO/vision mô phỏng: lưới 9 điểm X × Z, kiểm tra
    /// lưới, SET MASTER từng nozzle, tới điểm khắc đã bù, TEST MARK, trim.
    /// </summary>
    void CalibTests()
    {
        auto cal = McControl::Calib();
        auto fake = McControl::Vision()->Fake();
        auto sys = McDataManager::SysData();
        auto model = McDataManager::Model();
        CHECK(cal && fake && McControl::Vision()->IsSolutionLoaded());
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && St().OrgOk() && !McControl::RoutineBusy(); }));
        std::stop_token none;

        TeachAutoPoints();
        model->Calib = VisionCalib{};
        cal->Reset();
        sys->LampSettleMs = 0;
        sys->VisionSettleMs = 0;
        sys->VisionCalib = VisionCalibSettings{};
        sys->VisionCalib.ShotsPerPoint = 2;
        sys->VisionCalib.GridStepX = 4;
        sys->VisionCalib.GridStepZ = 3;
        sys->VisionCalib.CalibNozzle = 2;
        fake->SetProcessTimeMs(2);
        fake->SetFoundRate(1);
        for (int k = 1; k <= 7; ++k) fake->SetNozzleDeviation(k, 0, 0);
        fake->SetNozzleDeviation(2, 0.6, -0.3);
        const int pulses0 = Io().LaserPulseCount();

        // ── Cổng: chưa làm gì ──
        auto gate = cal->CheckCalib();
        CHECK_MSG(!gate.Ready() && gate.Block == "grid check not passed; nozzle 1, 2, 3, 4, 5, 6, 7 master not set", gate.Block);

        // ── Lưới 9 điểm X × Z ──
        auto r = cal->RunGridPoint(5, none);
        CHECK_MSG(!r.Ok && r.Text.find("grid center not set") != std::string::npos, r.Text);
        CHECK(McControl::Func()->Move(AxisId::X, 110, 200, none) && McControl::Func()->Move(AxisId::Z, 20, 100, none));
        r = cal->SaveGridCenter();
        CHECK_MSG(r.Ok && sys->VisionCalib.CenterSet && std::fabs(sys->VisionCalib.CenterX - 110) < 0.01 && std::fabs(sys->VisionCalib.CenterZ - 20) < 0.01, r.Text);
        r = cal->RunGridPoint(5, none);
        CHECK_MSG(!r.Ok && r.Text.find("nozzle 2 not holding a can") != std::string::npos, r.Text);
        Io().SetDo(IoMap::DoVacuum(2), true);   // can mẫu trên nozzle 2
        CHECK(WaitFor([] { return Io().GetDi(IoMap::VacuumSensor(2)); }));
        int pts = fake->CalibPointCount();
        std::vector<std::string> progress;
        // Clear Point không được (Flow không có module N-point): dừng TRƯỚC khi chạy trục, không nạp điểm nào.
        fake->SetFailClearPoint(true);
        r = cal->RunGrid([&](const std::string& s) { progress.push_back(s); }, none);
        CHECK_MSG(!r.Ok && StartsWith(r.Text, "Clear Point failed — no axis moved") && fake->CalibPointCount() == pts
                  && std::fabs(Ax(AxisId::X)->CurrentPos() - 110) < 0.01, r.Text);
        fake->SetFailClearPoint(false);
        progress.clear();
        int clears = fake->ClearPointCount();
        r = cal->RunGrid([&](const std::string& s) { progress.push_back(s); }, none);
        CHECK_MSG(r.Ok && fake->CalibPointCount() == pts + 9 && fake->ClearPointCount() == clears + 1 && fake->PointsSinceClear() == 9
                  && progress.size() == 10 && StartsWith(progress[0], "Clear Point"), r.Text);
        CHECK(fake->GetGlobal(sys->VisionOutputs.GvIsLocate) == 0);
        // Điểm cuối của hình S là số 9: góc trên phải của lưới (X +1 bước, Z +1 bước), gửi kèm số nozzle.
        CHECK(std::fabs(fake->LastCalibAxes()->X - 114) < 0.01 && std::fabs(fake->LastCalibAxes()->Z - 23) < 0.01 && fake->LastCalibAxes()->Nozzle == 2);
        r = cal->TriggerHere(none);
        CHECK_MSG(r.Ok && fake->CalibPointCount() == pts + 10, r.Text);

        // ── Kiểm tra lưới: can dính trên nozzle → Rel đứng yên khi trục chạy → đạt, ghi vào model, trục về chỗ cũ ──
        r = cal->VerifyGrid(none);
        CHECK_MSG(r.Ok && StartsWith(r.Text, "PASS") && model->Calib.GridVerified(), r.Text);
        CHECK(std::fabs(Ax(AxisId::X)->CurrentPos() - 114) < 0.01 && std::fabs(Ax(AxisId::Z)->CurrentPos() - 23) < 0.01);
        sys->VisionCalib.VerifyStepMm = 0.3;
        r = cal->VerifyGrid(none);
        CHECK_MSG(!r.Ok && r.Text.find("below 0.5 mm") != std::string::npos, r.Text);
        sys->VisionCalib.VerifyStepMm = 3;
        // Can "trôi" 0,4 mm mỗi ảnh → không đạt, không ghi đè.
        model->Calib.GridVerifiedAt.reset();
        fake->SetAfterFlow([&](const std::string&) { auto d = fake->NozzleDeviation(2); fake->SetNozzleDeviation(2, d.first + 0.4, d.second); });
        r = cal->VerifyGrid(none);
        fake->SetAfterFlow({});
        fake->SetNozzleDeviation(2, 0.6, -0.3);
        CHECK_MSG(!r.Ok && StartsWith(r.Text, "FAIL") && !model->Calib.GridVerified(), r.Text);
        CHECK(cal->VerifyGrid(none).Ok && model->Calib.GridVerified());

        // ── Master từng nozzle ──
        r = cal->GoToCam(2, none);
        CHECK_MSG(r.Ok && std::fabs(Ax(AxisId::X)->CurrentPos() - 310) < 0.01 && std::fabs(Ax(AxisId::Z)->CurrentPos() - 20) < 0.01, r.Text);
        CHECK(!cal->GoToCam(0, none).Ok && !cal->GoToCam(8, none).Ok);
        r = cal->Capture(2, none);
        CHECK_MSG(r.Ok && r.Text.find("master not set") != std::string::npos && cal->Last(2) && std::fabs(cal->Last(2)->RelX - 0.6) < 0.02
                  && std::fabs(cal->Last(2)->RelZ + 0.3) < 0.02 && !cal->Last(3), r.Text);
        r = cal->SetMaster(3, none);
        CHECK_MSG(!r.Ok && r.Text.find("nozzle 3 not holding a can") != std::string::npos, r.Text);
        model->Calib.Nozzle(2).TrimX = 0.3;   // trim cũ — phải bị xoá khi đo lại master
        r = cal->SetMaster(2, none);
        const auto& n2 = model->Calib.Nozzle(2);
        CHECK_MSG(r.Ok && n2.RefMeasured() && std::fabs(n2.RefX - 0.6) < 0.01 && std::fabs(n2.RefZ + 0.3) < 0.01 && n2.TrimX == 0
                  && r.Text.find("old trim cleared") != std::string::npos, r.Text);
        CHECK(cal->Last(2) && std::fabs(cal->Last(2)->Dx) < 0.01 && std::fabs(Ax(AxisId::X)->CurrentPos() - 310) < 0.01);
        auto g2 = VmCycle::EvaluateCalib(model.get(), sys.get(), 2);
        CHECK_MSG(g2.Ready(), g2.Block);
        gate = cal->CheckCalib();
        CHECK_MSG(!gate.Ready() && gate.Block == "nozzle 1, 3, 4, 5, 6, 7 master not set", gate.Block);

        // ── Thử laser: tới điểm Laser(2) + trim + bù theo lần soi gần nhất; TEST MARK kích DO ──
        CHECK(!cal->SetTrim(2, 1.5, 0).empty() && n2.TrimX == 0);
        CHECK(cal->SetTrim(2, 0.05, -0.04).empty() && n2.TrimX == 0.05 && n2.TrimZ == -0.04);
        r = cal->GoToLaser(2, none);
        CHECK_MSG(r.Ok, r.Text);
        CHECK_NEAR(Ax(AxisId::X)->CurrentPos(), 410.05, 0.02);
        CHECK_NEAR(Ax(AxisId::Z)->CurrentPos(), 24.96, 0.02);
        fake->SetNozzleDeviation(2, 0.8, -0.3);   // can lệch thêm +0.2 X so với master
        CHECK(cal->GoToCam(2, none).Ok && cal->Capture(2, none).Ok && std::fabs(cal->Last(2)->Dx - 0.2) < 0.02);
        r = cal->GoToLaser(2, none);
        CHECK_MSG(r.Ok && r.Text.find("no capture") == std::string::npos, r.Text);
        CHECK_NEAR(Ax(AxisId::X)->CurrentPos(), 409.85, 0.02);   // Gain −1: lùi đúng 0.2
        CHECK_NEAR(Ax(AxisId::Z)->CurrentPos(), 24.96, 0.02);
        r = cal->GoToLaser(4, none);   // chưa soi, chưa master: chỉ tới điểm teach
        CHECK_MSG(r.Ok && r.Text.find("no capture") != std::string::npos, r.Text);
        CHECK_NEAR(Ax(AxisId::X)->CurrentPos(), 430.0, 0.02);
        r = cal->TestMark(none);
        CHECK_MSG(r.Ok && StartsWith(r.Text, "Laser pulse 60 ms") && Io().LaserPulseCount() == pulses0 + 1 && !Io().GetDo(IoMap::DoLaserTrigger), r.Text);
        CHECK(cal->SetTrim(2, 0, 0).empty() && !cal->SetTrim(9, 0, 0).empty());
        Io().SetDo(IoMap::DoVacuum(2), false);
        CHECK(WaitFor([] { return !Io().GetDi(IoMap::VacuumSensor(2)); }));

        // ── Bản ghi đi theo model: lưu → nạp lại giữ nguyên ──
        auto copy = ModelData::FromJsonText(model->ToJsonText());
        CHECK(copy && copy->Calib.GridVerified() && copy->Calib.Nozzle(2).RefMeasured() && std::fabs(copy->Calib.Nozzle(2).RefX - n2.RefX) < 1e-9
              && !copy->Calib.Nozzle(3).RefMeasured());

        fake->SetNozzleDeviation(2, 0, 0);
        CHECK(McControl::Func()->EnsureZSafe(none));
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && !McControl::RoutineBusy(); }));
    }

    void FunctionTests()
    {
        auto f = McControl::Func();
        std::stop_source never;
        auto ct = never.get_token();
        std::string why;

        CHECK(f->Move(AxisId::Z, 50, 200, ct) && McControl::Motion().ZSafeOk() == false);
        f->Jog(AxisId::X, 1); Tick();
        CHECK_MSG(St().Message() == "Cannot jog X: Z not at safe height: Z=50.00 (need ≤ 11.00)", St().Message());
        CHECK(!f->Move(AxisId::X, 30, 100, ct));
        CHECK(f->EnsureZSafe(ct) && McControl::Motion().ZSafeOk());
        CHECK(f->CanMoveAutonomous(AxisId::X, why) && why.empty());
        double x0 = Ax(AxisId::X)->CurrentPos();
        CHECK(f->MoveInc(AxisId::X, 5, 100, ct));
        CHECK_NEAR(Ax(AxisId::X)->CurrentPos(), x0 + 5, 0.05);

        TeachPosition p; p.Pos = { { "X", 20.0 }, { "Y", 15.0 }, { "Z", 5.0 } };
        CHECK(f->MoveToPosition(&p, ct));
        CHECK_NEAR(Ax(AxisId::X)->CurrentPos(), 20.0, 0.05);
        CHECK_NEAR(Ax(AxisId::Y)->CurrentPos(), 15.0, 0.05);
        CHECK_NEAR(Ax(AxisId::Z)->CurrentPos(), 5.0, 0.05);
        CHECK(!f->MoveToPosition(nullptr, ct) && St().Message() == "Teach point has no coordinates");

        f->SetJogFast(true); CHECK(f->JogFast()); f->SetJogFast(false);
        f->Jog(AxisId::Y, 1); base::Delay(100); f->Jog(AxisId::Y, 0);
        CHECK(Ax(AxisId::Y)->CurrentPos() > 15.0);

        // Bit RUN tắt → lệnh tay bị từ chối.
        f->SetRunBit(false);
        f->Jog(AxisId::X, 1); Tick();
        CHECK(StartsWith(St().Message(), "Cannot jog X: machine paused"));
        CHECK(!f->CanMoveAutonomous(AxisId::X, why) && StartsWith(why, "machine paused"));
        f->SetRunBit(true);

        Ax(AxisId::Y)->SetServoOn(false);
        CHECK(!f->CanMove(AxisId::Y, why) && why == "Y servo OFF");
        CHECK(!f->CanMove("Q", why) && why == "no axis Q");
        Ax(AxisId::Y)->SetServoOn(true);

        CHECK(McFunction::SettleRemainingMs(McFunction::NeverMoved, 100, 200) == 0);
        CHECK(McFunction::SettleRemainingMs(1000, 1050, 200) == 150 && McFunction::SettleRemainingMs(1000, 1300, 200) == 0);
        CHECK(McFunction::UseTwoStageZ(0, 5, 10) && !McFunction::UseTwoStageZ(6, 5, 10) && McFunction::UseTwoStageZUp(10, 5, 0));

        // Buzzer: alarm Error kêu, nút Tắt còi (trên màn hình) tắt tiếng.
        AlarmManager::RaiseEvent(AlarmCode::RoutineCrashed, "Test", "buzzer");
        CHECK(WaitFor([] { return St().Buzzer() && Io().GetDo(IoMap::DoBuzzer) && Io().GetDo(IoMap::DoLampResetBtn); }));
        McControl::RequestMuteBuzzer();
        CHECK(WaitFor([] { return St().BuzzerMuted() && !St().Buzzer(); }));
        McControl::RequestReset();
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && !St().BuzzerMuted() && !Io().GetDo(IoMap::DoLampResetBtn); }));
    }

    void SafetyTests()
    {
        auto sys = McDataManager::SysData();
        // EMG.
        auto tokenBefore = McControl::Safety()->AbortToken();
        Io().SimSet(IoMap::EmgOk, false);
        CHECK(WaitFor([] { return St().State() == EMcState::Emg && St().TowerLamp() == ETowerLamp::RedBlink; }));
        CHECK(AlarmManager::IsActive(AlarmCode::EmergencyStop) && tokenBefore.stop_requested());
        CHECK(!McControl::Safety()->SafeToMove() && McControl::Safety()->BlockReason() == "EMG active" && !St().EmgOk());
        McControl::RequestReset(); Tick();
        CHECK(St().State() == EMcState::Emg && StartsWith(St().Message(), "RESET: cannot clear — "));
        Io().SimSet(IoMap::EmgOk, true); Tick();
        McControl::RequestReset();
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && McControl::Safety()->SafeToMove(); }));
        CHECK(!McControl::Safety()->AbortToken().stop_requested());

        // Rèm sáng: tắt trong SystemData thì không đọc.
        Io().SimSet(IoMap::LightCurtainOk, false); Tick();
        CHECK(St().LightCurtainOk() && McControl::Safety()->SafeToMove());
        sys->HasLightCurtain = true;
        CHECK(WaitFor([] { return !St().LightCurtainOk() && !McControl::Safety()->SafeToMove(); }));
        CHECK(McControl::Safety()->BlockReason() == "photo safety interrupted" && St().State() == EMcState::Idle && AlarmManager::ActiveCount() == 0);
        Io().SimSet(IoMap::LightCurtainOk, true);
        CHECK(WaitFor([] { return McControl::Safety()->SafeToMove(); }));
        sys->HasLightCurtain = false;

        // Áp suất khí: chỉ đọc khi máy có cảm biến; tụt áp → alarm 1103 (Error), hết tụt + RESET thì chạy lại.
        Io().SimSet(IoMap::AirOk, false); Tick();
        CHECK(St().AirOk() && McControl::Safety()->SafeToMove() && !AlarmManager::IsActive(AlarmCode::AirPressureLow));
        sys->HasAirSensor = true;
        CHECK(WaitFor([] { return !St().AirOk() && AlarmManager::IsActive(AlarmCode::AirPressureLow) && St().State() == EMcState::Alarm; }));
        CHECK(!McControl::Safety()->SafeToMove() && McControl::Safety()->BlockReason() == "alarm not cleared");   // alarm 1103 (Error) chặn trước
        Io().SimSet(IoMap::AirOk, true); Tick();
        McControl::RequestReset();
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && McControl::Safety()->SafeToMove() && St().AirOk(); }));
        sys->HasAirSensor = false;

        // Máy đứng: đèn STOP sáng, đèn START tắt.
        CHECK(WaitFor([] { return Io().GetDo(IoMap::DoLampStopBtn) && !Io().GetDo(IoMap::DoLampStartBtn); }));
        CHECK(!AlarmManager::IsActive(AlarmCode::ControlWatchdog));
    }

    void VisionTests()
    {
        auto sys = McDataManager::SysData();
        VisionOutputNames n = sys->VisionOutputs;
        CHECK(VmCycle::AxisVarProblem(nullptr) == "vision name config missing" && VmCycle::AxisVarProblem(&n).empty());
        VisionOutputNames bad = n; bad.GvAxisX.clear();
        CHECK(VmCycle::AxisVarProblem(&bad) == "axis variable names (X, Z) empty on Setting page");
        auto g = VmCycle::AxisGlobals(&McControl::Motion(), n, 42, 3);
        CHECK(g.size() == 4 && g[VmVars::RunId] == 42 && std::fabs(g[n.GvAxisX] - Ax(AxisId::X)->CurrentPos()) < 1e-9
              && std::fabs(g[n.GvAxisZ] - Ax(AxisId::Z)->CurrentPos()) < 1e-9 && g[n.GvNozzle] == 3);
        CHECK(VmCycle::AxisGlobals(nullptr, n, 1, 1)[n.GvAxisX] == 0);
        auto loc = VmCycle::LocateRequest(n, g);
        CHECK(loc.Flow == n.FlowLocateCan && loc.Camera == EVmCamera::Cam && loc.Outputs.size() == 1 + VmOut::LocateEcho().size()
              && loc.OptionalOutputs.size() == VmOut::LocateScript().size() && loc.Globals.at(n.GvIsLocate) == 1);   // nhánh soi
        CHECK(VmCycle::ExpectedFlowOutputs(n).size() == 1);   // LocateCan
        CHECK(!VmCycle::FakeFlowOutputs("NoSuchFlow", n).Error.empty() && VmCycle::FakeFlowOutputs(n.FlowLocateCan, n).Error.empty());

        CHECK(McControl::CheckVisionFlowOutputs().Error == "Vision solution not loaded");
        CHECK(McControl::SyncVisionGlobalVariables(false).Error == "Vision solution not loaded");
        sys->VisionSolutionPath = "sim.solw";
        int before = McControl::VisionLoadCount();
        CHECK(McControl::ReloadVisionSolution().empty());
        CHECK(WaitFor([&] { return McControl::VisionLoadCount() == before + 1 && McControl::Vision()->IsSolutionLoaded(); }));
        auto rep = McControl::CheckVisionFlowOutputs();
        CHECK_MSG(rep.Error.empty() && rep.Clean(), rep.ToText());
        auto sync = McControl::SyncVisionGlobalVariables(false);
        CHECK(sync.Error.empty() && sync.Count(EGlobalVarState::Missing) == 0 && sync.Count(EGlobalVarState::Existing) == 5);
        // Hợp đồng bị khai sai → báo không sạch.
        McControl::Vision()->Fake()->SetFlowOutputSource([](const std::string& f) { return VmFlowOutputs::Fail(f, "hỏng"); });
        CHECK(!McControl::CheckVisionFlowOutputs().Clean());
        sys->VisionSolutionPath.clear();
    }
}

int RunControllerTests()
{
    testkit::Group g("Controller");

    fs::path root = fs::temp_directory_path() / ("scs_ctrl_" + base::NowString("yyyyMMdd_HHmmssfff"));
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
    McDataManager::LoadRunningData();
    AlarmManager::ForceClearAll();
    auto sys = McDataManager::SysData();
    CHECK(sys->SimulationMode && AppPaths::Root() == root);
    sys->VisionSolutionPath.clear();
    FakeAxis::SetSpeedFactor(50);

    InitTests();
    ModeAndOriginTests();
    FunctionTests();
    SafetyTests();
    VisionTests();
    CalibTests();
    AutoCycleTests();

    McControl::Shutdown();
    CHECK(!McControl::IsInitialized());
    McControl::Init();
    CHECK(WaitFor([] { return St().State() == EMcState::NotHomed && St().IoConnected(); }));
    CHECK(!St().OrgOk() && St().Mode() == EMcMode::Manual && !McControl::RoutineBusy());
    RunOrigin();
    CHECK(St().OrgOk());
    McControl::Shutdown();
    CHECK(base::BackgroundTaskCount() == 0);

    FakeAxis::SetSpeedFactor(1);
    AlarmManager::ForceClearAll();
    LogWorker::Instance().Flush();
    return g.Result();
}
