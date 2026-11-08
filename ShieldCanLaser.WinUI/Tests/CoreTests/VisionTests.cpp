// Kiểm thử tầng vision: VmContract, đường dẫn kiểm tra (VisionOutputNames), VmCycle (phép thuần), FakeVision, VisionConnect.
// Vision chỉ kiểm tra sau khi khắc: một Flow.Group, một output Result (PLAN-LASER.md §4).
// Chạy trên thư mục dữ liệu tạm — không đụng D:\ShieldCanLaser, không nạp SDK VisionMaster.
#include "TestKit.h"

#include "Base/Async.h"
#include "Base/Text.h"
#include "Communication/VisionConnect.h"
#include "Controller/VmCycle.h"
#include "Infor/AlarmCatalog.h"
#include "Infor/AlarmManager.h"
#include "SysConfig/AppPaths.h"
#include "Utility/Log/LogWorker.h"

#include <cmath>
#include <filesystem>
#include <thread>

using namespace scs;
namespace fs = std::filesystem;

namespace
{
    bool StartsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

    /// Thiết bị "thô" giả cho VisionConnect: đếm lời gọi, treo theo lệnh, ném theo lệnh.
    struct StubDevice : IVisionDevice
    {
        std::atomic<bool> connected{ false }, loaded{ false };
        std::atomic<int> delayMs{ 0 }, calls{ 0 }, checks{ 0 };
        std::atomic<bool> throwOnCall{ false }, loadThrows{ false };
        std::shared_ptr<const ImageData> image = std::make_shared<ImageData>(ImageData{ 2, 1, 1, 2, { 7, 9 } });

        bool IsConnected() const override { return connected; }
        bool IsSolutionLoaded() const override { return loaded; }
        bool Connect() override { connected = true; return true; }
        void Disconnect() override { connected = false; loaded = false; }
        bool LoadSolution(const std::string& path) override
        {
            if (loadThrows) throw VisionDeviceError("SDK từ chối " + path);
            loaded = true;
            return true;
        }
        std::string CheckModulePath(const std::string& path) override
        {
            ++checks;
            if (throwOnCall) throw VisionDeviceError("boom");
            return path == "F.G" ? std::string() : "no " + path;
        }
        VmCallResult CallFlow(const VmCallRequest& request, std::stop_token) override
        {
            ++calls;
            base::Delay(delayMs);
            if (throwOnCall) throw VisionDeviceError("boom");
            VmCallResult r;
            r.Ok = true;
            r.Values["Result"] = request.Module == "F.G" ? 1 : 0;
            return r;
        }
        std::string SaveSolution() override { return "đĩa đầy"; }
        std::shared_ptr<const ImageData> LastImage() const override { return image; }
        int LastImageNozzle() const override { return 0; }
    };

    VmCallRequest Req(const std::string& flow, const std::string& module = {}, int nozzle = 0)
    {
        VmCallRequest r;
        r.Flow = flow;
        r.Module = module;
        r.Camera = EVmCamera::Cam;
        r.Nozzle = nozzle;
        r.Outputs.push_back("Result");
        return r;
    }

    VmCallResult Values(std::map<std::string, double> v)
    {
        VmCallResult r;
        r.Ok = true;
        r.Values = std::move(v);
        return r;
    }
}

int RunVisionTests()
{
    testkit::Group group("Vision");

    fs::path root = fs::temp_directory_path() / ("scs_vision_" + base::NowString("yyyyMMdd_HHmmssfff"));
    AppPaths::Init(base::PathToU8(root));
    LogWorker::Instance().Init(AppPaths::Root());
    fs::path realAlarms = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "Config" / "alarms.json";
    std::string tplText;
    if (base::ReadAllText(realAlarms, tplText))
    {
        fs::create_directories(root / "exe" / "Config");
        base::WriteAllText(root / "exe" / "Config" / "alarms.json", tplText);
        AppPaths::SetExeDirForTests(root / "exe");
    }
    AlarmCatalog::Load();
    AlarmManager::ForceClearAll();

    // ── VmContract ──
    VmCallResult vr;
    vr.Values["a"] = 2.5; vr.Values["n"] = std::nan("");
    CHECK(vr.Has("a") && !vr.Has("x") && std::isnan(vr.Get("x")) && vr.Get("a") == 2.5 && std::isnan(vr.Get("n")));
    VmCallResult vf = VmCallResult::Fail("vì sao", true);
    CHECK(!vf.Ok && vf.TimedOut && vf.Error == "vì sao" && !VmCallResult::Fail("x").TimedOut);
    CHECK(VmMath::SameInt(1.49, 1) && VmMath::SameInt(0.51, 1) && !VmMath::SameInt(1.5, 1) && !VmMath::SameInt(INFINITY, 1) && !VmMath::IsFinite(-INFINITY));

    // ── Đường dẫn kiểm tra Flow.Group ──
    VisionOutputNames n;
    CHECK(n.InspectPath == "Flow1.GroupInspection" && n.InspectFlow() == "Flow1" && n.InspectPathProblem().empty() && std::string(VisionOutputNames::OutResult) == "Result");
    n.InspectPath = " Inspect . Group A ";
    CHECK(n.InspectFlow() == "Inspect" && n.InspectPathProblem().empty());
    n.InspectPath = "Flow1.Outer.GroupInspection";   // Group lồng trong Group: Flow vẫn là phần trước dấu chấm đầu tiên
    CHECK(n.InspectFlow() == "Flow1" && n.InspectPathProblem().empty());
    n.InspectPath = "  ";
    CHECK(StartsWith(n.InspectPathProblem(), "inspection path not set"));
    n.InspectPath = "Flow1";
    CHECK(n.InspectPathProblem() == "inspection path \"Flow1\" has no Group — write Flow.Group, e.g. Flow1.GroupInspection");
    n.InspectPath = ".Group";
    CHECK(StartsWith(n.InspectPathProblem(), "inspection path \".Group\" must be Flow.Group"));
    n.InspectPath = "Flow1.";
    CHECK(StartsWith(n.InspectPathProblem(), "inspection path \"Flow1.\" must be Flow.Group"));
    CHECK(VisionOutputNames::RenderPath("Flow1", "") == "Flow1" && VisionOutputNames::RenderPath("Flow1", " Flow1.Img ") == "Flow1.Img");

    // ── VmCycle: lời gọi và cách đọc Result (phép thuần) ──
    VisionOutputNames def;
    def.InspectPath = " Flow1.GroupInspection ";
    auto req = VmCycle::InspectRequest(def, 4);
    CHECK(req.Flow == "Flow1" && req.Module == "Flow1.GroupInspection" && req.Camera == EVmCamera::Cam && req.Nozzle == 4
          && req.Outputs == std::vector<std::string>{ "Result" });
    auto ok = VmCycle::ReadInspect(Values({ { "Result", 1 } }));
    CHECK(ok.CallOk && ok.HasResult && ok.Pass && ok.Result == 1 && ok.Describe() == "OK (Result 1)");
    auto ng = VmCycle::ReadInspect(Values({ { "Result", 0 } }));
    CHECK(ng.CallOk && ng.HasResult && !ng.Pass && ng.Describe() == "NG (Result 0)");
    CHECK(!VmCycle::ReadInspect(Values({ { "Result", 2 } })).Pass && !VmCycle::ReadInspect(Values({ { "Result", -1 } })).Pass);
    CHECK(VmCycle::ReadInspect(Values({ { "Result", 0.999 } })).Pass);   // float 1 đi qua float32 vẫn là OK
    auto missing = VmCycle::ReadInspect(Values({}));
    CHECK(missing.CallOk && !missing.HasResult && !missing.Pass && missing.Describe() == "output Result missing or not a number");
    CHECK(!VmCycle::ReadInspect(Values({ { "Result", std::nan("") } })).HasResult);
    auto failed = VmCycle::ReadInspect(VmCallResult::Fail("Flow hỏng"));
    CHECK(!failed.CallOk && !failed.Pass && failed.Describe() == "Flow hỏng" && VmCycle::ReadInspect(VmCallResult::Fail("")).Describe() == "no result");
    CHECK(VmCycle::Fmt(1.5) == "1.5" && VmCycle::Fmt(-0.0001) == "0" && VmCycle::Fmt(2) == "2" && VmCycle::Fmt(std::nan("")) == "NaN (missing)");

    // ── FakeVision ──
    auto fake = std::make_shared<FakeVision>();
    fake->SetProcessTimeMs(1);
    CHECK(!fake->IsConnected() && fake->CallFlow(Req("Flow1"), {}).Error == "Not connected");
    CHECK(fake->Connect() && fake->IsConnected() && !fake->IsSolutionLoaded() && fake->CallFlow(Req("Flow1"), {}).Error == "Solution not loaded");
    CHECK(fake->CheckModulePath("Flow1.GroupInspection") == "Solution not loaded");
    CHECK(fake->LoadSolution("x.solw") && fake->IsSolutionLoaded());
    CHECK(fake->CheckModulePath(" Flow1.GroupInspection ").empty());
    CHECK(StartsWith(fake->CheckModulePath("Flow2.GroupInspection"), "Flow \"Flow2\" not found"));
    CHECK(StartsWith(fake->CheckModulePath("Flow1.Other"), "Module \"Flow1.Other\" not found in Flow \"Flow1\""));
    // Mặc định: mọi lần đều đạt.
    auto fr = fake->CallFlow(Req("Flow1", "Flow1.GroupInspection", 3), {});
    CHECK(fr.Ok && fr.Get("Result") == 1 && fake->InspectCount() == 1 && fake->LastImageNozzle() == 3 && !fake->LastImage());
    CHECK(fake->CallFlow(Req("Flow2", "Flow2.G"), {}).Error == "Flow \"Flow2\" not found in solution");
    CHECK(fake->CallFlow(Req("Flow1", "Flow1.X"), {}).Error == "Module \"Flow1.X\" not found in Flow \"Flow1\"");
    CHECK(fake->CallFlow(Req(""), {}).Error == "Missing Flow name" && fake->InspectCount() == 1);
    // Result đặt riêng từng nozzle; nozzle khác theo PassRate.
    fake->SetNozzleResult(2, 0);
    fake->SetNozzleResult(9, 0);   // ngoài 1…7: bỏ qua
    CHECK(fake->CallFlow(Req("Flow1", "Flow1.GroupInspection", 2), {}).Get("Result") == 0);
    CHECK(fake->CallFlow(Req("Flow1", "Flow1.GroupInspection", 1), {}).Get("Result") == 1);
    fake->SetPassRate(0);
    CHECK(fake->CallFlow(Req("Flow1", "Flow1.GroupInspection", 1), {}).Get("Result") == 0);
    fake->SetPassRate(1);
    fake->ClearNozzleResults();
    CHECK(fake->CallFlow(Req("Flow1", "Flow1.GroupInspection", 2), {}).Get("Result") == 1);
    // Lỗi một lần: Flow hỏng, hoặc Group không có Result.
    fake->SetFailNextCall(true);
    CHECK(fake->CallFlow(Req("Flow1", "Flow1.GroupInspection", 5), {}).Error == "Flow \"Flow1\" failed (simulated)" && !fake->FailNextCall());
    fake->SetDropResultOnce(true);
    CHECK(fake->CallFlow(Req("Flow1", "Flow1.GroupInspection", 5), {}).Error == "\"Flow1.GroupInspection\" missing output \"Result\"");
    CHECK(fake->CallFlow(Req("Flow1", "Flow1.GroupInspection", 5), {}).Ok);
    VmCallRequest other = Req("Flow1", "Flow1.GroupInspection");
    other.Outputs = { "Score" };
    CHECK(fake->CallFlow(other, {}).Error == "\"Flow1.GroupInspection\" missing output \"Score\"");
    CHECK((fake->CallHistorySnapshot() == std::vector<int>{ 3, 2, 1, 1, 2, 5, 5, 5, 0 }));
    fake->ClearCallHistory();
    CHECK(fake->CallHistorySnapshot().empty());
    // Móc sau Flow: chạy trước khi App đọc output, nhận đúng nozzle.
    int hooked = 0;
    fake->SetAfterFlow([&](int k) { hooked = k; });
    CHECK(fake->CallFlow(Req("Flow1", "Flow1.GroupInspection", 6), {}).Ok && hooked == 6);
    fake->SetAfterFlow({});
    // Huỷ trong lúc "chụp": ném OperationCanceled.
    {
        fake->SetProcessTimeMs(500);
        std::stop_source src;
        src.request_stop();
        bool canceled = false;
        try { fake->CallFlow(Req("Flow1", "Flow1.GroupInspection"), src.get_token()); }
        catch (const base::OperationCanceled&) { canceled = true; }
        CHECK(canceled);
        fake->SetProcessTimeMs(1);
    }
    // Tên cấu hình đọc qua móc: đổi đường dẫn là solution giả đổi theo.
    fake->SetNames([] { VisionOutputNames x; x.InspectPath = "Insp.G1"; return x; });
    CHECK(fake->CheckModulePath("Insp.G1").empty() && fake->CallFlow(Req("Insp", "Insp.G1"), {}).Ok && !fake->CallFlow(Req("Flow1", "Flow1.GroupInspection"), {}).Ok);
    CHECK(fake->SaveSolution().empty() && fake->SolutionSaveCount() == 1);
    fake->Disconnect();
    CHECK(!fake->IsConnected() && !fake->IsSolutionLoaded() && fake->CallFlow(Req("Insp", "Insp.G1"), {}).Error == "Not connected");

    // ── VisionConnect: mô phỏng từ đầu tới cuối ──
    VisionConnect::SetRealDeviceFactory({});
    {
        auto sys = std::make_shared<SystemData>();
        sys->SimulationMode = true;
        sys->VisionOutputs.InspectPath = "Custom.GroupX";
        VisionConnect vc(sys);
        CHECK(vc.IsSimulation() && vc.Fake() && vc.Device() == vc.Fake() && !vc.IsConnected() && vc.CallFlow(Req("Custom", "Custom.GroupX"), {}).Error == "Not connected");
        vc.Fake()->SetProcessTimeMs(5);
        CHECK(vc.Connect() && vc.IsConnected() && !vc.IsSolutionLoaded() && vc.LoadSolution("sim.solw") && vc.IsSolutionLoaded());
        auto r = VmCycle::ReadInspect(vc.CallFlow(VmCycle::InspectRequest(sys->VisionOutputs, 2), {}));
        CHECK(r.Pass && vc.LastImageNozzle() == 2);
        CHECK(vc.CheckModulePath("Custom.GroupX").empty() && StartsWith(vc.CheckModulePath("Flow1.GroupInspection"), "Flow \"Flow1\" not found"));
        CHECK(vc.CallFlow(Req("Flow1", "Flow1.GroupInspection"), {}).Error == "Flow \"Flow1\" not found in solution");
        CHECK(vc.SaveSolution().empty() && !vc.LastImage());
        vc.Disconnect();
        CHECK(!vc.IsConnected() && !vc.IsSolutionLoaded() && AlarmManager::ActiveCount() == 0);
    }

    // ── VisionConnect: máy thật mà chưa đăng ký thiết bị → alarm 4001 + log, KHÔNG rơi về bản giả ──
    auto sys = std::make_shared<SystemData>();
    sys->SimulationMode = false;
    sys->VisionTriggerTimeoutMs = 150;
    {
        VisionConnect vc(sys);
        CHECK(!VisionConnect::HasRealDeviceFactory() && !vc.IsSimulation() && !vc.Fake() && !vc.Device());
        CHECK(!vc.Connect() && !vc.IsConnected() && AlarmManager::IsActive(AlarmCode::VisionLoadFailed));
        CHECK(AlarmManager::Get(AlarmCode::VisionLoadFailed)->Detail == "Real VisionMaster device not registered (VisionConnect::SetRealDeviceFactory)" && AlarmManager::Get(AlarmCode::VisionLoadFailed)->Source == "Vision");
        AlarmManager::ForceClearAll();
        CHECK(!vc.LoadSolution("a.solw") && !vc.IsSolutionLoaded() && AlarmManager::IsActive(AlarmCode::VisionLoadFailed));
        CHECK(vc.CallFlow(Req("Flow1"), {}).Error == "Not connected" && vc.CheckModulePath("Flow1.G") == "Not connected");
        CHECK(vc.SaveSolution() == "Solution not loaded" && !vc.LastImage());
        LogWorker::Instance().Flush();
        bool logged = false;
        for (auto& l : LogWorker::Instance().LogList())
            if (l.Message == "Vision: load solution failed — Real VisionMaster device not registered (VisionConnect::SetRealDeviceFactory)" && l.Source == "Vision" && l.Level == LogLevel::Error) logged = true;
        CHECK(logged);
        AlarmManager::ForceClearAll();
    }

    // ── VisionConnect: thiết bị thô qua hàm tạo đăng ký ──
    auto stub = std::make_shared<StubDevice>();
    VisionConnect::SetRealDeviceFactory([stub]() -> std::shared_ptr<IVisionDevice> { return stub; });

    // Mô phỏng trục/IO mà có VisionMaster thật: vẫn dùng thiết bị thật, trừ khi chọn vision giả.
    {
        auto simSys = std::make_shared<SystemData>();
        CHECK(simSys->SimulationMode && !simSys->VisionSimulation);
        VisionConnect real(simSys);
        CHECK(!real.IsSimulation() && !real.Fake() && real.Connect() && real.Device() == stub && real.LoadSolution("s.solw") && real.IsSolutionLoaded());
        real.Disconnect();
        CHECK(!stub->connected);
        simSys->VisionSimulation = true;
        VisionConnect fakeVc(simSys);
        CHECK(fakeVc.IsSimulation() && fakeVc.Fake() && fakeVc.Device() == fakeVc.Fake());
        simSys->SimulationMode = false;   // máy thật: cờ vision giả không có tác dụng
        VisionConnect machine(simSys);
        CHECK(!machine.IsSimulation() && !machine.Fake());
        AlarmManager::ForceClearAll();
    }
    {
        VisionConnect vc(sys);
        CHECK(vc.Connect() && vc.IsConnected() && vc.Device() == stub && !vc.IsSolutionLoaded() && vc.CallFlow(Req("F"), {}).Error == "Solution not loaded");
        CHECK(vc.CheckModulePath("F.G") == "Solution not loaded" && stub->checks == 0);
        stub->loadThrows = true;
        CHECK(!vc.LoadSolution("b.solw") && !AlarmManager::IsActive(AlarmCode::VisionLoadFailed));
        stub->loadThrows = false;
        CHECK(vc.LoadSolution("b.solw") && vc.IsSolutionLoaded());
        auto okCall = vc.CallFlow(Req("F", "F.G"), {});
        CHECK(okCall.Ok && okCall.Get("Result") == 1 && stub->calls == 1 && vc.LastImage() == stub->image && vc.LastImageNozzle() == 0);
        CHECK(vc.CallFlow(Req(""), {}).Error == "Missing Flow name" && stub->calls == 1);
        // Kiểm đường dẫn: tới thiết bị (giữ khoá lệnh, không canh giờ); trống thì không gọi.
        CHECK(vc.CheckModulePath("F.G").empty() && vc.CheckModulePath("F.X") == "no F.X" && stub->checks == 2);
        CHECK(vc.CheckModulePath("  ") == "Path not set on Setting page" && stub->checks == 2);
        CHECK(vc.SaveSolution() == "đĩa đầy");

        stub->throwOnCall = true;
        CHECK(vc.CallFlow(Req("F"), {}).Error == "F: boom" && AlarmManager::ActiveCount() == 0);
        CHECK(vc.CheckModulePath("F.G") == "boom");
        stub->throwOnCall = false;

        // Quá giờ: alarm 4003 dạng sự kiện; lần gọi xếp hàng sau một Run treo bị bỏ thì KHÔNG chạy muộn.
        stub->delayMs = 500;
        int before = stub->calls;
        base::Stopwatch sw;
        auto t1 = vc.CallFlow(Req("Slow"), {});
        CHECK(!t1.Ok && t1.TimedOut && t1.Error == "Timeout waiting for Flow Slow" && sw.ElapsedMilliseconds() < 450);
        auto alarm = AlarmManager::Get(AlarmCode::VisionTriggerTimeout);
        CHECK(alarm && alarm->Detail == "Slow" && alarm->Source == "Vision" && !alarm->ConditionActive);
        auto t2 = vc.CallFlow(Req("Queued"), {});
        CHECK(t2.TimedOut && t2.Error == "Timeout waiting for Flow Queued");
        base::Delay(600);
        CHECK(stub->calls == before + 1);
        AlarmManager::ForceClearAll();

        // Huỷ giữa lúc Flow chạy: không phải quá giờ, không alarm.
        {
            std::stop_source src;
            std::jthread killer([&] { base::Delay(40); src.request_stop(); });
            auto c = vc.CallFlow(Req("Cut"), src.get_token());
            CHECK(!c.Ok && !c.TimedOut && c.Error == "Canceled while running Flow Cut" && AlarmManager::ActiveCount() == 0);
        }
        base::Delay(600);
        stub->delayMs = 0;
        CHECK(vc.CallFlow(Req("F", "F.G"), {}).Get("Result") == 1);
        vc.Disconnect();
        CHECK(!vc.IsConnected() && !stub->connected && vc.CallFlow(Req("F"), {}).Error == "Not connected");
    }

    VisionConnect::SetRealDeviceFactory({});
    base::WaitBackgroundIdle(3000);
    AlarmManager::ForceClearAll();
    AlarmManager::FlushCsv();
    AppPaths::SetExeDirForTests({});
    LogWorker::Instance().Shutdown();
    std::error_code ec;
    fs::remove_all(root, ec);
    return group.Result();
}
