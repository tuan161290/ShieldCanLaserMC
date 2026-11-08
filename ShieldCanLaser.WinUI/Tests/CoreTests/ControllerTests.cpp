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
                bool on = Io().GetDo(IoMap::LaserStart);
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
        CHECK(WaitFor([] { return St().TowerLamp() == ETowerLamp::Yellow && Io().GetDo(IoMap::DoTowerLampYellow) && Io().GetDo(IoMap::DoLampStopBtn); }));
        CHECK(St().EmgOk() && St().SafetyChainOk() && McControl::Safety()->SafeToMove() && McControl::Safety()->BlockReason().empty());
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
        McControl::RequestReset();                         // máy không có nút RESET cứng: RESET trên màn hình
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
        model->Trim = LaserTrim{};
    }

    /// Chu trình Laser Marking đủ vòng trên trục/IO/vision mô phỏng: gắp cả hàng, khắc từng nozzle tại điểm Laser(k) + trim,
    /// rồi CAM kiểm tra từng can vừa khắc (Result 1 = OK), trả về tray; hút hụt → ô NG không khắc; kiểm tra NG → ô NG;
    /// không kiểm tra được → ô NG + cảnh báo 5006; hết tray → chờ START thay tray; START không có tray → từ chối; STOP sau
    /// khi gắp → trả hàng về chưa khắc; STOP lúc kiểm tra → xong hàng rồi mới dừng.
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
        fake->SetPassRate(1);
        fake->ClearNozzleResults();
        fake->SetNozzleResult(3, 0);   // can trên nozzle 3 khắc hỏng
        Io().SimSetPickFail(5, true);
        Io().SimSet(IoMap::TrayPresent(1), true);
        Io().SimSet(IoMap::TrayPresent(2), true);
        const int pulses0 = Io().LaserPulseCount();
        const int inspect0 = fake->InspectCount();

        McControl::RequestSetMode(EMcMode::Auto);
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        CHECK(WaitFor([] { return Io().GetDo(IoMap::DoLampStartBtn) && Io().GetDo(IoMap::DoLampStartBtn2); }));   // cả hai đèn START nhấp nháy

        // Van hút còn mở từ lần trước → không chạy, báo rõ nozzle.
        Io().SetDo(IoMap::DoVacuum(2), true);
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }));
        CHECK_MSG(StartsWith(St().Message(), "Cycle stopped: Nozzle 2 vacuum still ON"), St().Message());
        Io().SetDo(IoMap::DoVacuum(2), false);
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));

        // Van thổi bật tay còn mở → không chạy: nozzle hạ xuống sẽ thổi văng can trong tray.
        Io().SetDo(IoMap::VacuumBlower(3), true);
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }));
        CHECK_MSG(StartsWith(St().Message(), "Cycle stopped: Nozzle 3 blower still ON"), St().Message());
        Io().SetDo(IoMap::VacuumBlower(3), false);
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));

        // Đường dẫn kiểm tra sai → dừng ngay từ đầu, chưa khắc can nào (khắc cả tray rồi mới NG hết là phí).
        auto& names = McDataManager::SysData()->VisionOutputs;
        const std::string path = names.InspectPath;
        names.InspectPath = "Flow1";
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }));
        CHECK_MSG(StartsWith(St().Message(), "Cycle stopped: Vision: inspection path \"Flow1\" has no Group") && Io().LaserPulseCount() == pulses0, St().Message());
        names.InspectPath = path;
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));

        int blow0[8]{};
        for (int k = 1; k <= 7; ++k) blow0[k] = Io().DoRiseCount(IoMap::VacuumBlower(k));

        // ── Lượt 1: 2 tray × 2 hàng, nozzle 5 hút hụt, can trên nozzle 3 kiểm tra NG ──
        {
            LaserProbe probe;
            Io().SimPress(IoMap::BtnStart);
            CHECK(WaitFor([&] { return r->WaitingTray && St().State() == EMcState::WaitOperator && St().AutoStep() == EAutoState::S60_WaitTray; }, 60000));
            CHECK_MSG(run->Trays[0].DoneCount(14) == 14 && run->Trays[1].DoneCount(14) == 14 && run->CountOk == 20 && run->CountNg == 8 && run->CountTotal == 28, St().Message());
            // N3 vẫn được khắc (kiểm tra là SAU khi khắc); N5 hút hụt thì không khắc, không kiểm tra.
            CHECK(Io().LaserPulseCount() == pulses0 + 24 && fake->InspectCount() == inspect0 + 24);
            CHECK_MSG(St().Message().find("Trays done — replace trays, then press START") != std::string::npos
                      && StartsWith(St().Message(), "Tray 2 row 2 done: 5 OK, 2 NG"), St().Message());
            CHECK(run->LastRowTray == 2 && run->LastRowIndex == 2 && run->LastRowResult.size() == 7 && run->LastRowResult[4] == (int)ECellState::Ng
                  && run->LastRowResult[2] == (int)ECellState::Ng && run->LastRowResult[0] == (int)ECellState::Ok);
            // Tray 2 vừa xong: chu kỳ đã đóng, giữ số cuối; 2 hàng × 5 can OK.
            CHECK(run->CycleStartedAtMs == 0 && run->CycleHoldLast && run->LastCycleSec > 0 && run->LastCycleProducts == 10);
            CHECK(std::fabs(Ax(AxisId::X)->CurrentPos() - 10) < 0.05 && std::fabs(Ax(AxisId::Y)->CurrentPos() - 10) < 0.05 && McControl::Motion().ZSafeOk());
            for (int k = 1; k <= 7; ++k)
            {
                CHECK(!Io().GetDo(IoMap::DoVacuum(k)) && !Io().GetDo(IoMap::VacuumBlower(k)));
                // Thổi nhả mỗi lần trả hàng (4 hàng), trừ nozzle 5 hút hụt: thổi sẽ hất can còn trong ô.
                CHECK_MSG(Io().DoRiseCount(IoMap::VacuumBlower(k)) == blow0[k] + (k == 5 ? 0 : 4), std::format("blower N{}", k));
            }
            CHECK(!Io().GetDo(IoMap::DoLightCam) && !Io().GetDo(IoMap::LaserStart) && !Io().GetDo(IoMap::LaserStop));
            CHECK(Io().DoRiseCount(IoMap::VacuumBlower(IoMap::VacuumChannelCount)) == 0 && Io().DoRiseCount(IoMap::DoVacuum(IoMap::VacuumChannelCount)) == 0);   // kênh 8 dự phòng
            CHECK(WaitFor([] { return Io().GetDo(IoMap::DoLampStopBtn) && Io().GetDo(IoMap::DoLampStartBtn); }));   // chờ START: STOP sáng, START nháy
            // Kiểm tra NG là ô NG thường, không phải alarm.
            CHECK(AlarmManager::IsActive(AlarmCode::PickFailed) && AlarmManager::IsActive(AlarmCode::TrayEmpty) && !AlarmManager::IsActive(AlarmCode::VisionNg)
                  && !AlarmManager::HasError());

            // Điểm khắc = điểm teach, không bù: N1 (400, 25), N3 (420, 25), N4 (430, 25), N7 (460, 25); N5 không khắc.
            auto shots = probe.Shots();
            CHECK_MSG(shots.size() == 24, std::to_string(shots.size()));
            if (shots.size() == 24)
            {
                CHECK_NEAR(shots[0].first, 400.0, 0.03); CHECK_NEAR(shots[0].second, 25.0, 0.03);
                CHECK_NEAR(shots[2].first, 420.0, 0.03); CHECK_NEAR(shots[2].second, 25.0, 0.03);
                CHECK_NEAR(shots[3].first, 430.0, 0.03); CHECK_NEAR(shots[5].first, 460.0, 0.03);
                CHECK_NEAR(shots[6].first, 400.0, 0.03);   // hàng 2 lại từ N1
            }

            auto view = Routine_Auto::Trays();
            CHECK(view.Trays[0].size() == 14 && view.Trays[1].size() == 14);
            for (int t = 0; t < 2; ++t)
                for (int i = 0; i < 14; ++i)
                    CHECK_MSG(view.Trays[t][i] == (i % 7 == 4 || i % 7 == 2 ? (int)ECellState::Ng : (int)ECellState::Ok), std::format("tray {} cell {}", t, i));
            auto noz = Routine_Auto::Nozzles();
            CHECK(noz[3].HasResult && noz[3].Result == 0 && noz[3].State == (int)ECellState::Empty && noz[3].Text == "returned (NG)");
            CHECK(noz[1].HasResult && noz[1].Result == 1 && noz[1].Text == "returned (OK)");
            CHECK(noz[5].State == (int)ECellState::Empty && noz[5].Text == "pick missed" && !noz[5].HasResult);
            CHECK_MSG(Routine_Auto::ProgressText().find("Tray 1: 14/14 cells · Tray 2: 14/14 cells done") != std::string::npos
                      && Routine_Auto::ProgressText().find("N5·NG") != std::string::npos && Routine_Auto::ProgressText().find("N3·NG") != std::string::npos,
                      Routine_Auto::ProgressText());
        }

        // ── START lúc chờ mà không có tray → từ chối, vẫn chờ ──
        Io().SimSet(IoMap::TrayPresent(1), false);
        Io().SimSet(IoMap::TrayPresent(2), false);
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().Message() == "START rejected: no tray detected (X008/X009)"; }));
        CHECK(r->WaitingTray && St().State() == EMcState::WaitOperator);

        // ── Lượt 2: chỉ tray 1; lần kiểm tra đầu tiên Flow lỗi → ô NG + cảnh báo 5006; N3 vẫn NG; nozzle 5 hút lại được ──
        Io().SimSetPickFail(5, false);
        fake->SetFailNextCall(true);
        Io().SimSet(IoMap::TrayPresent(1), true);
        Io().SimPress(IoMap::BtnStart2);                                                        // nút START #2 cùng tác dụng START
        CHECK(WaitFor([&] { return !r->WaitingTray && St().State() == EMcState::Running; }));   // đã nhận tray mới, chạy lại từ hàng 1
        CHECK(WaitFor([&] { return r->WaitingTray && St().State() == EMcState::WaitOperator; }, 60000));
        CHECK_MSG(run->Trays[0].DoneCount(14) == 14 && run->Trays[1].DoneCount(14) == 0 && run->CountOk == 31 && run->CountNg == 11, St().Message());
        CHECK(Io().LaserPulseCount() == pulses0 + 38 && fake->InspectCount() == inspect0 + 38);
        CHECK(AlarmManager::IsActive(AlarmCode::VisionNg));
        {
            auto view = Routine_Auto::Trays();
            CHECK(view.Trays[1].size() == 14 && view.Trays[1][0] == (int)ECellState::Loaded && view.Trays[0][0] == (int)ECellState::Ng
                  && view.Trays[0][2] == (int)ECellState::Ng && view.Trays[0][4] == (int)ECellState::Ok && view.Trays[0][7] == (int)ECellState::Ok
                  && view.Trays[0][9] == (int)ECellState::Ng);
            auto noz = Routine_Auto::Nozzles();
            CHECK_MSG(noz[3].State == (int)ECellState::Empty && noz[3].Text == "returned (NG)" && noz[3].HasResult && noz[3].Result == 0, noz[3].Text);
        }
        fake->ClearNozzleResults();

        // ── STOP lúc chờ thay tray → thoát chu trình, máy ĐÃ DỪNG ──
        McControl::RequestStop();
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }));
        CHECK(!r->WaitingTray);
        CHECK(WaitFor([] { return !Io().GetDo(IoMap::DoLampStartBtn); }));

        // ── STOP lúc đang gắp: trả cả hàng về tray rồi mới dừng — hàng chưa khắc, chưa tính, lần sau chạy lại ──
        Io().SimSet(IoMap::TrayPresent(2), true);
        // Cảnh báo của lượt trước (5001/5006/5020) phải RESET xác nhận rồi mới vào sẵn sàng lại.
        McControl::RequestReset();
        CHECK(WaitFor([] { return AlarmManager::ActiveCount() == 0 && St().Message() == "RESET done"; }));
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        const int inspect1 = fake->InspectCount();
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().AutoStep() == EAutoState::S20_PickRow; }, 10000));
        McControl::RequestPause();
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }, 30000));
        CHECK(run->Trays[0].DoneCount(14) == 14 && run->Trays[1].DoneCount(14) == 0 && run->CountOk == 31 && run->CountNg == 11
              && Io().LaserPulseCount() == pulses0 + 38 && fake->InspectCount() == inspect1);
        for (int k = 1; k <= 7; ++k) CHECK(!Io().GetDo(IoMap::DoVacuum(k)) && !Io().GetDi(IoMap::VacuumSensor(k)));
        CHECK(McControl::Motion().ZSafeOk());
        {
            auto view = Routine_Auto::Trays();
            CHECK(view.Trays[1].size() == 14 && view.Trays[1][0] == (int)ECellState::Loaded && view.Trays[1][6] == (int)ECellState::Loaded);
        }

        // ── STOP lúc đang kiểm tra: can đã khắc nên kiểm tra và trả xong cả hàng rồi mới dừng (khắc lại là khắc đè) ──
        std::atomic<int> flows{ 0 };
        fake->SetAfterFlow([&](int) { if (flows++ == 0) McControl::RequestPause(); });
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }, 30000));
        fake->SetAfterFlow({});
        CHECK_MSG(run->Trays[1].DoneCount(14) == 7 && run->CountOk == 38 && run->CountNg == 11 && Io().LaserPulseCount() == pulses0 + 45
                  && fake->InspectCount() == inspect1 + 7, St().Message());
        for (int k = 1; k <= 7; ++k) CHECK(!Io().GetDo(IoMap::DoVacuum(k)));

        // ── Chạy tiếp: tray 2 hàng 2, kiểm tra NG cả hàng → 7 ô NG, đã khắc, không alarm ──
        fake->SetPassRate(0);
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([&] { return r->WaitingTray && St().State() == EMcState::WaitOperator; }, 60000));
        CHECK_MSG(run->Trays[1].DoneCount(14) == 14 && run->CountOk == 38 && run->CountNg == 18 && Io().LaserPulseCount() == pulses0 + 52, St().Message());
        CHECK(!AlarmManager::IsActive(AlarmCode::VisionNg) && StartsWith(St().Message(), "Tray 2 row 2 done: 0 OK, 7 NG"));
        {
            auto noz = Routine_Auto::Nozzles();
            CHECK_MSG(noz[1].HasResult && noz[1].Result == 0 && noz[1].Text == "returned (NG)", noz[1].Text);
        }
        fake->SetPassRate(1);

        // ── Tắt vision + tắt laser: chỉ gắp, tới điểm khắc (+ trim), trả về — không kiểm tra, không kích ──
        McDataManager::Model()->Auto.UseVision = false;
        McDataManager::Model()->Auto.UseLaser = false;
        Io().SimSet(IoMap::TrayPresent(2), false);
        const int inspect2 = fake->InspectCount();
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([&] { return !r->WaitingTray; }));
        CHECK(WaitFor([&] { return r->WaitingTray && St().State() == EMcState::WaitOperator; }, 60000));
        CHECK_MSG(run->Trays[0].DoneCount(14) == 14 && run->CountOk == 52 && run->CountNg == 18 && Io().LaserPulseCount() == pulses0 + 52
                  && fake->InspectCount() == inspect2, St().Message());
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
        CHECK(run->CycleStartedAtMs != 0);   // dừng giữa tray: đồng hồ vẫn chạy, RESET TRAY mới bỏ
        CHECK(McControl::ResetTrays().empty() && run->Trays[0].DoneCount(14) == 0 && run->Trays[1].DoneCount(14) == 0 && run->LastRowResult.empty());
        CHECK(St().Message() == "Tray progress reset — next cycle starts from row 1 of tray 1");
        CHECK(run->CycleStartedAtMs == 0);

        McControl::RequestSetMode(EMcMode::Manual);
        CHECK(WaitFor([] { return St().Mode() == EMcMode::Manual; }));
        McControl::RequestReset();
        CHECK(WaitFor([] { return AlarmManager::ActiveCount() == 0; }));
        Io().SimSet(IoMap::TrayPresent(1), true);
    }

    /// Chạy lẻ: chỉ các ô chọn mới hút, khắc, kiểm tra và thổi, mỗi lượt một hàng; tray không chọn cũng không chạy; xong về vị trí chờ,
    /// máy về Idle. Ô không chọn chạy tiếp được, ô đã khắc không khắc lại kể cả trong cùng hàng. Khoá chọn lúc chạy; tray vắng;
    /// hút hụt chỉ tính ô chọn; chọn trước cho tray mới; RESET TRAY.
    void CellSelectionTests()
    {
        auto r = McControl::AutoRoutine();
        auto run = McDataManager::RunData();
        auto fake = McControl::Vision()->Fake();
        auto model = McDataManager::Model();
        CHECK(WaitFor([] { return St().Mode() == EMcMode::Manual && !McControl::RoutineBusy(); }));
        model->Auto.TrayRows = 3;   // mỗi tray 21 ô
        CHECK(McControl::ResetTrays().empty());
        CHECK(Routine_Auto::Trays().Trays[0].empty());   // sơ đồ cũ không còn: trang Main suy lại, không tô OK/NG ô đã đặt lại
        Io().SimSet(IoMap::TrayPresent(1), true);
        Io().SimSet(IoMap::TrayPresent(2), true);
        Tick();

        int vac[8]{}, blow[8]{};
        auto snapshot = [&] {
            for (int k = 1; k <= 7; ++k)
            {
                vac[k] = Io().DoRiseCount(IoMap::DoVacuum(k));
                blow[k] = Io().DoRiseCount(IoMap::VacuumBlower(k));
            }
        };

        // MANUAL không chọn được.
        CHECK(!McControl::CanSelectCells());
        McControl::ToggleCellSelection(0, 8);
        McControl::SetCellsSelected(0, { 8, 9 }, true);
        CHECK(!run->HasCellSelection());

        McControl::RequestSetMode(EMcMode::Auto);
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        CHECK(McControl::CanSelectCells());
        McControl::ToggleCellSelection(0, 8);            // tray 1 ô 2-2
        McControl::ToggleCellSelection(0, 11);           // tray 1 ô 2-5
        McControl::ToggleCellSelection(1, 20);           // tray 2 ô 3-7
        McControl::ToggleCellSelection(1, 0);
        McControl::ToggleCellSelection(1, 0);            // chạm lần hai = bỏ chọn
        McControl::ToggleCellSelection(0, 21);           // ngoài tray
        McControl::ToggleCellSelection(2, 0);            // không có tray 3
        CHECK(run->Trays[0].Selected() == (std::vector<int>{ 8, 11 }) && run->Trays[1].Selected() == (std::vector<int>{ 20 }));

        // ── Lượt lẻ: 2 ô hàng 2 tray 1, 1 ô hàng 3 tray 2 — đúng 3 can, chỉ van và laser của các ô đó ──
        snapshot();
        fake->ClearCallHistory();
        const int pulses0 = Io().LaserPulseCount();
        const int inspect0 = fake->InspectCount();
        const int ok0 = run->CountOk, ng0 = run->CountNg, total0 = run->CountTotal;
        model->Auto.MarkTimeMs = 300;                    // đủ lâu để còn đang chạy lúc thử khoá
        {
            LaserProbe probe;
            Io().SimPress(IoMap::BtnStart);
            CHECK(WaitFor([] { return St().State() == EMcState::Running; }));
            // Đang chạy: khoá. Bỏ chọn lúc này mà có tác dụng thì máy khắc cả tray (số xung cuối lượt sẽ khác).
            CHECK(McControl::CellSelectionLocked() && !McControl::CanSelectCells());
            CHECK(WaitFor([&] { return run->CycleStartedAtMs != 0 && !run->CycleHoldLast; }));   // máy Running trước, đồng hồ bắt đầu ở hàng đầu tiên
            McControl::ClearCellSelection();
            McControl::ToggleCellSelection(0, 0);
            McControl::SetCellsSelected(0, { 0, 1 }, true);
            McControl::SetCellsSelected(1, { 20 }, false);
            CHECK(!run->Trays[0].IsSelected(0) && !run->Trays[0].IsSelected(1));
            CHECK(WaitFor([] { return St().State() == EMcState::Idle && !McControl::RoutineBusy(); }, 60000));
            CHECK_MSG(StartsWith(St().Message(), "Tray 2 row 3 done: 1 OK, 0 NG")
                      && St().Message().find("Selected cells done — press RUN, then START") != std::string::npos, St().Message());
            auto shots = probe.Shots();                  // khắc đúng N2, N5 rồi N7
            CHECK_MSG(shots.size() == 3, std::to_string(shots.size()));
            if (shots.size() == 3)
            {
                CHECK_NEAR(shots[0].first, 410.0, 0.03);
                CHECK_NEAR(shots[1].first, 440.0, 0.03);
                CHECK_NEAR(shots[2].first, 460.0, 0.03);
            }
        }
        model->Auto.MarkTimeMs = 60;
        CHECK(Io().LaserPulseCount() == pulses0 + 3 && fake->InspectCount() == inspect0 + 3);
        CHECK((fake->CallHistorySnapshot() == std::vector<int>{ 2, 5, 7 }));   // kiểm tra đúng nozzle vừa khắc
        CHECK(run->CountOk == ok0 + 3 && run->CountNg == ng0 && run->CountTotal == total0 + 3);   // ô không chạy không tính NG
        CHECK(run->CycleStartedAtMs == 0 && run->CycleHoldLast && run->LastCycleProducts == 3 && run->LastCycleSec > 0);   // cả lượt lẻ là một chu kỳ
        for (int k = 1; k <= 7; ++k)
        {
            const int expected = k == 2 || k == 5 || k == 7 ? 1 : 0;
            CHECK_MSG(Io().DoRiseCount(IoMap::DoVacuum(k)) == vac[k] + expected, std::format("vacuum N{}", k));
            CHECK_MSG(Io().DoRiseCount(IoMap::VacuumBlower(k)) == blow[k] + expected, std::format("blower N{}", k));
            CHECK(!Io().GetDo(IoMap::DoVacuum(k)) && !Io().GetDo(IoMap::VacuumBlower(k)));
        }
        CHECK(run->Trays[0].Done() == (std::vector<int>{ 8, 11 }) && run->Trays[1].Done() == (std::vector<int>{ 20 }) && !run->HasCellSelection());
        CHECK(std::fabs(Ax(AxisId::X)->CurrentPos() - 10) < 0.05 && std::fabs(Ax(AxisId::Y)->CurrentPos() - 10) < 0.05 && McControl::Motion().ZSafeOk());
        CHECK(!r->WaitingTray);
        {
            auto view = Routine_Auto::Trays();
            CHECK(view.Trays[0].size() == 21 && view.Trays[1].size() == 21);
            CHECK(view.Trays[0][7] == (int)ECellState::Loaded && view.Trays[0][8] == (int)ECellState::Ok && view.Trays[0][9] == (int)ECellState::Loaded
                  && view.Trays[0][11] == (int)ECellState::Ok && view.Trays[0][0] == (int)ECellState::Loaded);
            CHECK(view.Trays[1][19] == (int)ECellState::Loaded && view.Trays[1][20] == (int)ECellState::Ok);
            CHECK(run->LastRowResult.size() == 7 && run->LastRowResult[6] == (int)ECellState::Ok && run->LastRowResult[0] == (int)ECellState::Empty);
            const std::string progress = Routine_Auto::ProgressText();
            CHECK_MSG(progress.find("Tray 1: 2/21 cells · Tray 2: 1/21 cells done") != std::string::npos
                      && progress.find("N1·—") != std::string::npos && progress.find("N7·OK") != std::string::npos, progress);
        }

        // START theo thói quen ngay sau lượt lẻ: không chạy, phải bấm RUN trước.
        Io().SimPress(IoMap::BtnStart);
        Tick();
        CHECK(St().State() == EMcState::Idle && !McControl::RoutineBusy() && Io().LaserPulseCount() == pulses0 + 3);

        // ── Hút hụt chỉ tính ô chọn: N3 chọn mà hụt = NG; N5 không chọn nên hụt cũng không sao ──
        Io().SimSetPickFail(3, true);
        Io().SimSetPickFail(5, true);
        McControl::ToggleCellSelection(0, 2);            // tray 1 ô 1-3
        McControl::ToggleCellSelection(0, 3);            // tray 1 ô 1-4
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().State() == EMcState::Running; }));
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && !McControl::RoutineBusy(); }, 60000));
        CHECK_MSG(StartsWith(St().Message(), "Tray 1 row 1 done: 1 OK, 1 NG"), St().Message());
        CHECK(Io().LaserPulseCount() == pulses0 + 4 && run->CountNg == ng0 + 1 && run->CountOk == ok0 + 4);
        CHECK(run->LastCycleProducts == 1);   // chỉ tính can OK
        CHECK(run->Trays[0].Done() == (std::vector<int>{ 2, 3, 8, 11 }));
        {
            auto view = Routine_Auto::Trays();
            CHECK(view.Trays[0][2] == (int)ECellState::Ng && view.Trays[0][3] == (int)ECellState::Ok && view.Trays[0][4] == (int)ECellState::Loaded);
        }
        Io().SimSetPickFail(3, false);
        Io().SimSetPickFail(5, false);
        McControl::RequestReset();
        CHECK(WaitFor([] { return AlarmManager::ActiveCount() == 0; }));

        // Kéo chọn vùng: chỉ lấy ô chọn được (8, 11 đã chạy); bỏ chọn cả vùng; cả vùng không được thì báo lý do.
        McControl::SetCellsSelected(0, { 7, 8, 9, 10, 11, 12, 21 }, true);
        CHECK(run->Trays[0].Selected() == (std::vector<int>{ 7, 9, 10, 12 }));
        CHECK(McControl::CanSelectCell(0, 9) && !McControl::CanSelectCell(0, 8) && !McControl::CanSelectCell(0, 21) && !McControl::CanSelectCell(2, 0));
        McControl::SetCellsSelected(0, { 9, 10 }, false);
        CHECK(run->Trays[0].Selected() == (std::vector<int>{ 7, 12 }));
        McControl::ClearCellSelection();
        McControl::SetCellsSelected(0, { 8, 11 }, true);
        CHECK_MSG(!run->HasCellSelection() && St().Message() == "Tray 1 cell 2-2 already run — RESET TRAY to run it again", St().Message());

        // Ô đã chạy không chọn được: khắc lại là đè lên can đã khắc.
        McControl::ToggleCellSelection(0, 8);
        CHECK_MSG(!run->HasCellSelection() && St().Message() == "Tray 1 cell 2-2 already run — RESET TRAY to run it again", St().Message());

        // Tray vắng: không chọn được; chọn rồi mới mất tray thì START dừng báo, không âm thầm bỏ ô, ô chọn giữ nguyên.
        Io().SimSet(IoMap::TrayPresent(2), false);
        Tick();
        McControl::ToggleCellSelection(1, 0);
        CHECK_MSG(!run->HasCellSelection() && St().Message() == "Tray 2 not detected (X009) — cannot select its cells", St().Message());
        Io().SimSet(IoMap::TrayPresent(2), true);
        Tick();
        McControl::ToggleCellSelection(0, 0);
        McControl::ToggleCellSelection(1, 0);
        Io().SimSet(IoMap::TrayPresent(2), false);
        Tick();
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().State() == EMcState::Paused && !McControl::RoutineBusy(); }));
        CHECK_MSG(StartsWith(St().Message(), "Cycle stopped: Tray 2 has selected cells but is not detected (X009)"), St().Message());
        CHECK(Io().LaserPulseCount() == pulses0 + 4 && run->Trays[1].IsSelected(0) && run->Trays[0].IsSelected(0));
        Io().SimSet(IoMap::TrayPresent(2), true);
        McControl::ClearCellSelection();
        CHECK(!run->HasCellSelection());

        // ── Bỏ chọn: START chạy nốt 37 ô chưa chạy; hàng đã có ô xong thì chỉ hút các ô còn lại, không khắc đè ──
        snapshot();
        McControl::RequestArm();
        CHECK(WaitFor([] { return St().State() == EMcState::Armed; }));
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([&] { return r->WaitingTray && St().State() == EMcState::WaitOperator; }, 60000));
        CHECK(Io().LaserPulseCount() == pulses0 + 4 + 37);
        CHECK(run->Trays[0].DoneCount(21) == 21 && run->Trays[1].DoneCount(21) == 21);
        CHECK(run->LastCycleProducts == 20 && run->CycleStartedAtMs == 0);   // mỗi tray một chu kỳ: tray 2 còn 20 ô
        const int expectedRows[8] = { 0, 6, 5, 5, 5, 5, 6, 5 };
        for (int k = 1; k <= 7; ++k)
            CHECK_MSG(Io().DoRiseCount(IoMap::DoVacuum(k)) == vac[k] + expectedRows[k], std::format("vacuum N{}", k));

        // ── Chờ thay tray: chọn được (kể cả ô đã chạy) cho tray mới; START chạy đúng ô đó rồi dừng ──
        CHECK(McControl::CanSelectCells() && !McControl::CellSelectionLocked());
        McControl::ToggleCellSelection(0, 16);
        CHECK(run->Trays[0].IsSelected(16));
        Io().SimPress(IoMap::BtnStart);
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && !McControl::RoutineBusy(); }, 60000));
        CHECK(Io().LaserPulseCount() == pulses0 + 4 + 37 + 1);
        CHECK(run->LastCycleProducts == 1);
        CHECK(run->Trays[0].Done() == (std::vector<int>{ 16 }) && run->Trays[1].Done().empty() && !run->HasCellSelection() && !r->WaitingTray);

        // RESET TRAY lúc không chờ: ô đã chạy và ô chọn đều xoá.
        McControl::ToggleCellSelection(1, 1);
        CHECK(McControl::ResetTrays().empty() && run->Trays[0].Done().empty() && !run->HasCellSelection());

        McControl::RequestSetMode(EMcMode::Manual);
        CHECK(WaitFor([] { return St().Mode() == EMcMode::Manual; }));
        McControl::RequestReset();
        CHECK(WaitFor([] { return AlarmManager::ActiveCount() == 0; }));
        model->Auto.TrayRows = 2;
    }

    /// <summary>
    /// Các nút thử của trang Vision Edit (Routine_Calib) trên trục/IO/vision mô phỏng: tới điểm khắc + trim, TEST MARK, trim,
    /// tới CAM, kiểm tra một can trên nozzle.
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
        cal->Reset();
        sys->LampSettleMs = 0;
        sys->VisionSettleMs = 0;
        fake->SetProcessTimeMs(2);
        fake->SetPassRate(1);
        fake->ClearNozzleResults();
        const int pulses0 = Io().LaserPulseCount();

        // ── Thử khắc: tới điểm Laser(2) + trim; TEST MARK kích DO ──
        const auto& t2 = model->Trim.Nozzle(2);
        CHECK(!cal->SetTrim(2, 1.5, 0).empty() && t2.X == 0);
        CHECK(cal->SetTrim(2, 0.05, -0.04).empty() && t2.X == 0.05 && t2.Z == -0.04);
        auto r = cal->GoToLaser(2, none);
        CHECK_MSG(r.Ok && r.Text.find("trim +0.050/-0.040") != std::string::npos, r.Text);
        CHECK_NEAR(Ax(AxisId::X)->CurrentPos(), 410.05, 0.02);
        CHECK_NEAR(Ax(AxisId::Z)->CurrentPos(), 24.96, 0.02);
        r = cal->GoToLaser(4, none);   // không trim: đúng điểm teach (nội suy N1…N7)
        CHECK_MSG(r.Ok, r.Text);
        CHECK_NEAR(Ax(AxisId::X)->CurrentPos(), 430.0, 0.02);
        CHECK(!cal->GoToLaser(0, none).Ok && !cal->GoToLaser(8, none).Ok);
        const int stops0 = Io().DoRiseCount(IoMap::LaserStop);
        r = cal->TestMark(none);
        CHECK_MSG(r.Ok && StartsWith(r.Text, "Laser pulse 60 ms") && Io().LaserPulseCount() == pulses0 + 1 && !Io().GetDo(IoMap::LaserStart), r.Text);
        CHECK(Io().DoRiseCount(IoMap::LaserStop) == stops0);   // khắc trọn vẹn: không có xung dừng

        // Lệnh khắc bị cắt ngang: LaserStart rớt, LaserStop một xung rồi về 0. Thời gian khắc dài và chỉ huỷ khi đã thấy
        // LaserStart lên, nên lệnh huỷ luôn rơi đúng lúc đang khắc dù máy bận.
        {
            const int markMs = model->Auto.MarkTimeMs;
            model->Auto.MarkTimeMs = 10000;
            std::stop_source cut;
            std::jthread canceler([&] {
                while (!Io().GetDo(IoMap::LaserStart)) base::Delay(1);
                cut.request_stop();
            });
            r = cal->TestMark(cut.get_token());
            model->Auto.MarkTimeMs = markMs;
        }
        CHECK_MSG(!r.Ok && Io().DoRiseCount(IoMap::LaserStop) == stops0 + 1, r.Text);
        CHECK(!Io().GetDo(IoMap::LaserStart) && !Io().GetDo(IoMap::LaserStop));

        // ── Thử kiểm tra: tới CAM, chạy Flow tại chỗ (đèn CAM bật lúc chụp), đọc Result của Group ──
        r = cal->GoToCam(2, none);
        CHECK_MSG(r.Ok && std::fabs(Ax(AxisId::X)->CurrentPos() - 310) < 0.01 && std::fabs(Ax(AxisId::Z)->CurrentPos() - 20) < 0.01, r.Text);
        CHECK(!cal->GoToCam(0, none).Ok && !cal->GoToCam(8, none).Ok);
        r = cal->Inspect(2, none);
        CHECK_MSG(!r.Ok && r.Text.find("nozzle 2 not holding a can") != std::string::npos && !cal->LastInspect(2), r.Text);
        Io().SetDo(IoMap::DoVacuum(2), true);   // can đã khắc trên nozzle 2
        CHECK(WaitFor([] { return Io().GetDi(IoMap::VacuumSensor(2)); }));
        bool lightAtShot = false;
        fake->SetAfterFlow([&](int) { lightAtShot = Io().GetDo(IoMap::DoLightCam); });
        r = cal->Inspect(2, none);
        fake->SetAfterFlow({});
        CHECK_MSG(r.Ok && r.Text == "Nozzle 2 · Flow1.GroupInspection: OK (Result 1)" && cal->LastInspect(2) && cal->LastInspect(2)->Pass
                  && lightAtShot && !Io().GetDo(IoMap::DoLightCam), r.Text);
        fake->SetNozzleResult(2, 0);
        r = cal->Inspect(2, none);
        CHECK_MSG(r.Ok && r.Text.find("NG (Result 0)") != std::string::npos && !cal->LastInspect(2)->Pass, r.Text);
        // Group không trả Result → nút báo không đạt kèm lý do.
        fake->SetDropResultOnce(true);
        r = cal->Inspect(2, none);
        CHECK_MSG(!r.Ok && r.Text.find("missing output \"Result\"") != std::string::npos && !cal->LastInspect(2)->CallOk, r.Text);
        // Đường dẫn chưa đủ Flow.Group → không chạy Flow.
        const std::string path = sys->VisionOutputs.InspectPath;
        sys->VisionOutputs.InspectPath = "";
        const int calls = fake->InspectCount();
        r = cal->Inspect(2, none);
        CHECK_MSG(!r.Ok && r.Text.find("inspection path not set") != std::string::npos && fake->InspectCount() == calls, r.Text);
        sys->VisionOutputs.InspectPath = path;
        fake->ClearNozzleResults();
        CHECK(!cal->LastInspect(3) && !cal->LastInspect(0));
        cal->Reset();
        CHECK(!cal->LastInspect(2));

        CHECK(cal->SetTrim(2, 0, 0).empty() && !cal->SetTrim(9, 0, 0).empty());
        Io().SetDo(IoMap::DoVacuum(2), false);
        CHECK(WaitFor([] { return !Io().GetDi(IoMap::VacuumSensor(2)); }));

        // ── Trim đi theo model: lưu → nạp lại giữ nguyên ──
        CHECK(cal->SetTrim(6, -0.2, 0.3).empty());
        auto copy = ModelData::FromJsonText(model->ToJsonText());
        CHECK(copy && copy->Trim.Nozzle(6).X == -0.2 && copy->Trim.Nozzle(6).Z == 0.3 && copy->Trim.Nozzle(2).X == 0);
        CHECK(cal->SetTrim(6, 0, 0).empty());

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

        // Phanh Z: servo Z bật thì nhả; tắt servo thì phanh đóng TRƯỚC, servo tắt sau; đảo tay lúc servo tắt bị vòng quét ghi đè.
        auto z = Ax(AxisId::Z);
        CHECK(WaitFor([] { return Io().GetDo(IoMap::ZAxisBrakeRelease); }));
        {
            bool brakeBeforeServoOff = false;
            std::jthread off([z] { McControl::ServoOff(z); });
            base::Stopwatch sw;
            while (z->ServoOn() && sw.ElapsedMilliseconds() < 2000)
            {
                if (!Io().GetDo(IoMap::ZAxisBrakeRelease)) brakeBeforeServoOff = true;
                base::Delay(1);
            }
            CHECK(brakeBeforeServoOff);
        }
        CHECK(!z->ServoOn());
        Tick();
        CHECK(!Io().GetDo(IoMap::ZAxisBrakeRelease));
        Io().SetDo(IoMap::ZAxisBrakeRelease, true);
        CHECK(WaitFor([] { return !Io().GetDo(IoMap::ZAxisBrakeRelease); }));
        z->SetServoOn(true);
        CHECK(WaitFor([] { return Io().GetDo(IoMap::ZAxisBrakeRelease); }));

        CHECK(McFunction::SettleRemainingMs(McFunction::NeverMoved, 100, 200) == 0);
        CHECK(McFunction::SettleRemainingMs(1000, 1050, 200) == 150 && McFunction::SettleRemainingMs(1000, 1300, 200) == 0);
        CHECK(McFunction::UseTwoStageZ(0, 5, 10) && !McFunction::UseTwoStageZ(6, 5, 10) && McFunction::UseTwoStageZUp(10, 5, 0));

        // Buzzer: alarm Error kêu, nút Tắt còi (trên màn hình) tắt tiếng.
        AlarmManager::RaiseEvent(AlarmCode::RoutineCrashed, "Test", "buzzer");
        CHECK(WaitFor([] { return St().Buzzer() && Io().GetDo(IoMap::DoBuzzer); }));
        McControl::RequestMuteBuzzer();
        CHECK(WaitFor([] { return St().BuzzerMuted() && !St().Buzzer(); }));
        McControl::RequestReset();
        CHECK(WaitFor([] { return St().State() == EMcState::Idle && !St().BuzzerMuted(); }));
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

        // Máy đứng: đèn STOP sáng, hai đèn START tắt.
        CHECK(WaitFor([] { return Io().GetDo(IoMap::DoLampStopBtn) && !Io().GetDo(IoMap::DoLampStartBtn) && !Io().GetDo(IoMap::DoLampStartBtn2); }));
        CHECK(!AlarmManager::IsActive(AlarmCode::ControlWatchdog));
    }

    void VisionTests()
    {
        auto sys = McDataManager::SysData();
        VisionOutputNames n = sys->VisionOutputs;
        auto req = VmCycle::InspectRequest(n, 3);
        CHECK(req.Flow == "Flow1" && req.Module == "Flow1.GroupInspection" && req.Camera == EVmCamera::Cam && req.Nozzle == 3
              && req.Outputs == std::vector<std::string>{ "Result" });

        CHECK(McControl::CheckInspectionPath() == "Vision solution not loaded");
        sys->VisionSolutionPath = "sim.solw";
        int before = McControl::VisionLoadCount();
        CHECK(McControl::ReloadVisionSolution().empty());
        CHECK(WaitFor([&] { return McControl::VisionLoadCount() == before + 1 && McControl::Vision()->IsSolutionLoaded(); }));
        CHECK(McControl::CheckInspectionPath().empty());
        // Đường dẫn sai: không đủ Flow.Group, hoặc Group không có trong Flow.
        sys->VisionOutputs.InspectPath = "Flow1";
        CHECK_MSG(StartsWith(McControl::CheckInspectionPath(), "inspection path \"Flow1\" has no Group"), McControl::CheckInspectionPath());
        // (Flow/Group không có trong solution: VisionTests kiểm ở FakeVision — bản giả ở đây lấy "solution" theo chính đường dẫn đang cài.)
        sys->VisionOutputs.InspectPath = n.InspectPath;
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
    CellSelectionTests();

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
