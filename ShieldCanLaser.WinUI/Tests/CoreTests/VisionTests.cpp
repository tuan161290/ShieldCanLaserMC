// Kiểm thử tầng vision: VmContract, VmOutCheck, GlobalVarSync, FakeVision, VisionConnect, VmCycle (phép thuần).
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

#include <climits>
#include <cmath>
#include <filesystem>
#include <thread>

using namespace scs;
namespace fs = std::filesystem;

namespace
{
    struct ThrowingStore : IGlobalVarStore
    {
        std::map<std::string, std::string> ReadAll() override { throw std::runtime_error("boom"); }
        void Add(const std::string&, const std::string&, const std::string&) override {}
        void SetValue(const std::string&, const std::string&, const std::string&) override {}
        std::string Save() override { return {}; }
    };

    /// Thiết bị "thô" giả cho VisionConnect: đếm lời gọi, treo theo lệnh, ném theo lệnh.
    struct StubDevice : IVisionDevice
    {
        std::atomic<bool> connected{ false }, loaded{ false };
        std::atomic<int> delayMs{ 0 }, calls{ 0 }, calibCalls{ 0 }, clearCalls{ 0 };
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
        CalibClearResult ClearCalibPoints(EVisionProcedure) override
        {
            ++clearCalls;
            if (throwOnCall) throw VisionDeviceError("boom");
            CalibClearResult r;
            r.Modules.push_back("N-point Calibration1");
            return r;
        }
        VisionPose TriggerCalib(EVisionProcedure, const AxisSnapshot& axes, std::stop_token) override
        {
            ++calibCalls;
            base::Delay(delayMs);
            VisionPose p;
            p.Found = axes.X > 0;
            return p;
        }
        GlobalVarSyncReport SyncGlobalVariables(const std::vector<GlobalVarSpec>&, bool) override { throw VisionDeviceError("sync hỏng"); }
        VmFlowOutputs ReadFlowOutputs(const std::string& flow) override
        {
            VmFlowOutputs r;
            r.Flow = flow;
            r.AddDeclared("ObjX_mm", 14);
            r.AddDeclared("Img", 1);
            return r;
        }
        VmCallResult CallFlow(const VmCallRequest& request, std::stop_token) override
        {
            ++calls;
            base::Delay(delayMs);
            if (throwOnCall) throw VisionDeviceError("boom");
            VmCallResult r;
            r.Ok = true;
            r.Values["Echo"] = request.Globals.count("G") ? request.Globals.at("G") : -1;
            return r;
        }
        std::string SaveSolution() override { return "đĩa đầy"; }
        std::shared_ptr<const ImageData> LastImage() const override { return image; }
        int LastImageNozzle() const override { return 0; }
    };

    VmCallRequest Req(const std::string& flow, std::map<std::string, double> globals, std::vector<std::string> outs, std::vector<std::string> opt = {})
    {
        VmCallRequest r;
        r.Flow = flow;
        r.Camera = EVmCamera::Cam;
        r.Globals = std::move(globals);
        r.Outputs = std::move(outs);
        r.OptionalOutputs = std::move(opt);
        return r;
    }

    VmMeanCapture Cap(double relX, double relZ, double axX, double axZ)
    {
        VmMeanCapture c;
        c.Ok = true; c.Shots = 1;
        c.AxX = axX; c.AxZ = axZ; c.RelX = relX; c.RelZ = relZ; c.X = axX + relX; c.Y = axZ + relZ;
        return c;
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
    vr.Values["a"] = 2.5; vr.Values["b"] = 3.5; vr.Values["c"] = -1.4; vr.Values["n"] = std::nan("");
    CHECK(vr.Has("a") && !vr.Has("x") && std::isnan(vr.Get("x")) && vr.Get("c") == -1.4);
    CHECK(vr.GetInt("a") == 2 && vr.GetInt("b") == 4 && vr.GetInt("c") == -1 && vr.GetInt("x") == INT_MIN && vr.GetInt("n") == INT_MIN);
    VmCallResult vf = VmCallResult::Fail("vì sao", true);
    CHECK(!vf.Ok && vf.TimedOut && vf.Error == "vì sao" && !VmCallResult::Fail("x").TimedOut);
    CHECK(VmMath::Near(1.0, 1.0009) && !VmMath::Near(1.0, 1.0011) && !VmMath::Near(std::nan(""), 1) && VmMath::Near(1, 2, 1.0));
    CHECK(VmMath::SameInt(2.49, 2) && !VmMath::SameInt(2.5, 2) && !VmMath::SameInt(INFINITY, 2) && !VmMath::IsFinite(-INFINITY));
    int id1 = VmRunIds::Next(), id2 = VmRunIds::Next();
    CHECK(id1 >= 1 && id1 <= VmRunIds::Max && (id2 == id1 + 1 || (id1 == VmRunIds::Max && id2 == 1)));
    CHECK(VmOut::LocateEcho().size() == 3 && VmOut::LocateEcho()[2] == "EchoZ_mm" && VmOut::LocateScript().size() == 4 && VmOut::LocateScript()[3] == "ObjRunId");
    CHECK(VmStatus::GridVerify(2) == "travel below 0.5 mm" && VmStatus::GridVerify(9) == "Unknown Status (9)"
          && VmStatus::GridVerify(8).rfind("WRONG SCALE/SIGN", 0) == 0);

    // ── VmOutCheck ──
    CHECK(VmOutCheck::TypeName(13) == "int" && VmOutCheck::TypeName(14) == "float" && VmOutCheck::TypeName(1) == "image" && VmOutCheck::TypeName(99) == "type 99");
    CHECK(VmOutCheck::IsNumeric(13) && !VmOutCheck::IsNumeric(25) && VmOutCheck::CleanIoName("%ObjX_mm%") == "ObjX_mm" && VmOutCheck::CleanIoName("%%") == "%%");
    CHECK(VmOutCheck::Normalize(" ObjX_mm ") == "objx" && VmOutCheck::Normalize("Echo_Z_mm") == "echoz" && VmOutCheck::EditDistance("detal", "delta") == 2);
    CHECK(VmOutCheck::SuggestFor("ObjX", { "EchoX_mm", "ObjX_mm" }) == std::optional<std::string>("ObjX_mm") && !VmOutCheck::SuggestFor("Dbg", { "ObjX_mm" }));
    {
        VmFlowExpect e; e.Flow = "F0"; e.Names = { "A_mm", "B" }; e.ImageName = "ImageData";
        VmFlowOutputs a; a.Flow = "F0"; a.AddDeclared("A_mm", 14); a.AddDeclared("B", 13); a.AddDeclared("ImageData", 1); a.InputParamCount = 0;
        VmOutCheckReport rep;
        VmOutCheck::CompareFlow(e, &a, rep);
        CHECK(rep.Clean() && rep.Count(EVmOutState::Ok) == 2 && rep.Items.size() == 2);
        CHECK_MSG(rep.ToText().rfind("OK: 2 · Missing: 0\r\n", 0) == 0, rep.ToText());
    }
    {
        VmFlowExpect e; e.Flow = "F1"; e.Names = { "ObjX_mm", "ObjY_mm", "ObjRunId", "ObjFound" }; e.Note = "nhắc";
        VmFlowOutputs a; a.Flow = "F1"; a.AddDeclared("ObjX", 14); a.AddDeclared("ObjY_mm", 15); a.AddDeclared("ObjFound", 13); a.AddDeclared("Dbg", 14); a.InputParamCount = 2;
        VmOutCheckReport rep;
        VmOutCheck::CompareFlow(e, &a, rep);
        CHECK(!rep.Clean() && rep.Count(EVmOutState::Missing) == 2 && rep.Count(EVmOutState::Typo) == 1 && rep.Count(EVmOutState::WrongType) == 1 && rep.Count(EVmOutState::Extra) == 1);
        std::string want =
            "OK: 1 · Missing: 2 · Wrong name: 1 · Wrong type: 1 · Extra: 1\r\n"
            "\r\nFlow F1:\r\n"
            "  · missing output ObjX_mm\r\n"
            "  · ObjY_mm declared as string — App reads only float or int, will report \"missing output\"\r\n"
            "  · missing output ObjRunId\r\n"
            "  · found \"ObjX\" — contract uses \"ObjX_mm\", rename this field\r\n"
            "\r\nExtra, not in contract (App ignores, may keep):\r\n"
            "  · F1: Dbg\r\n"
            "\r\n· F1 still declares 2 Input Params — contract no longer uses Input Params, delete them\r\n· F1: nhắc";
        CHECK_MSG(rep.ToText() == want, rep.ToText());

        VmOutCheckReport errs;
        VmFlowOutputs bad = VmFlowOutputs::Fail("F1", "không có Flow"), empty;
        VmOutCheck::CompareFlow(e, nullptr, errs);
        VmOutCheck::CompareFlow(e, &bad, errs);
        VmOutCheck::CompareFlow(e, &empty, errs);
        CHECK(errs.FlowErrors.size() == 3 && errs.FlowErrors[1] == "F1: không có Flow" && errs.FlowErrors[2].find("F1: solution returned no output names") == 0);
        CHECK(!errs.Clean() && errs.ToText().find("OK: 0 · Missing: 0\r\n✗ F1: cannot read\r\n✗ F1: không có Flow\r\n") == 0);
        CHECK(VmOutCheckReport::Fail("chưa nạp").ToText() == "ERROR: chưa nạp" && empty.InputParamCount == -1);
    }
    {
        VmScriptExpect e; e.Flow = "LC"; e.ScriptName = "CanAbs"; e.Inputs = { "EchoX_mm", "MatchNum" }; e.Outputs = { "ObjFound" };
        VmFlowOutputs f; f.Flow = "LC";
        VmOutCheckReport rep;
        VmOutCheck::CompareScript(e, &f, rep);
        CHECK(rep.Notes.size() == 1 && rep.Notes[0] == "LC: cannot browse modules inside Flow — Script check skipped");
        f.Modules.push_back(VmModuleIo{ "Script9", {}, { { "Foo", "float", "" } } });
        VmOutCheck::CompareScript(e, &f, rep);
        CHECK(rep.FlowErrors.size() == 1 && rep.FlowErrors[0] == "LC: no module declares the outputs of Script CanAbs (modules in Flow: Script9) — Script9 declares: Foo");
        f.Modules.push_back(VmModuleIo{ "S1", { { "EchoX_mm", "float", "Echo0.X" }, { "MatchNum", "int", "" } }, { { "ObjFound", "float", "" } } });
        VmOutCheckReport rep2;
        VmOutCheck::CompareScript(e, &f, rep2);
        CHECK(VmOutCheck::FindScript(&f, e)->Name == "S1" && rep2.Count(EVmOutState::Unbound) == 1 && rep2.Count(EVmOutState::Ok) == 2 && !rep2.Clean());
        CHECK(rep2.Items.size() == 3 && rep2.Items[2].Flow == "LC · Script S1 (Input Params)" && rep2.Items[2].Name == "MatchNum");
        CHECK(rep2.ToText().find("  · MatchNum UNBOUND — Subscribe field empty, Script input always 0\r\n") != std::string::npos && rep2.ToText().find(" · UNBOUND: 1") != std::string::npos);
        auto scripts = VmScriptContract::Build(VisionOutputNames());
        CHECK(scripts.size() == 1 && scripts[0].Flow == "LocateCan" && scripts[0].ScriptName == "CanAbs" && scripts[0].Inputs.size() == 4
              && scripts[0].Outputs.size() == 4 && scripts[0].Outputs[1] == "ObjX_mm");
    }

    // ── GlobalVarSync (kho biến của FakeVision) ──
    std::string why;
    CHECK(GlobalVarSync::IsValidName("X_mm", why) && !GlobalVarSync::IsValidName(" ", why) && why == "empty name");
    CHECK(!GlobalVarSync::IsValidName("a ", why) && why == "leading/trailing spaces" && !GlobalVarSync::IsValidName(std::string(126, 'a'), why) && why == "longer than 125 bytes");
    CHECK(GlobalVarSync::IsValidName(std::string(125, 'a'), why) && !GlobalVarSync::IsValidName("a(b)", why));
    ThrowingStore throwing;
    CHECK(GlobalVarSync::Run(&throwing, {}, false).ToText() == "ERROR: cannot read global variable list — boom");
    CHECK(GlobalVarSync::Run(nullptr, {}, true).Error == "variable store or variable list missing");
    auto catalog = VisionGlobalVarCatalog::Build(nullptr);
    // 5 biến: X_mm, Z_mm, Nozzle, RunId (float) + biến rẽ nhánh IsLocate (int, giá trị đầu 1).
    CHECK(catalog.size() == 5 && catalog[0].Name == "X_mm" && catalog[1].Name == "Z_mm" && catalog[2].Name == "Nozzle" && catalog[3].Name == "RunId"
          && catalog[4].Name == "IsLocate" && catalog[4].Type == "int" && catalog[4].InitialValue == "1" && catalog[0].Type == "float" && catalog[0].InitialValue == "0");
    {
        VisionOutputNames noBranch; noBranch.GvIsLocate.clear();
        CHECK(VisionGlobalVarCatalog::Build(noBranch).size() == 4);
    }
    CHECK(catalog[3].Remark == "App writes before every Flow — call number, echoed by Flow");

    auto fake = std::make_shared<FakeVision>();
    fake->SetProcessTimeMs(5);
    std::vector<GlobalVarSpec> specs{ { "A", "float", "0.5", "", {} }, { "B", "float", "0", "", {} }, { "C", "float", "0.25", "r", {} }, { "bad.name", "float", "0", "", {} }, { "A", "int", "", "", {} } };
    CHECK(fake->SyncGlobalVariables(specs, false).Error == "Solution not loaded" && fake->SaveSolution() == "Solution not loaded");
    CHECK(!fake->LoadSolution("x.solw") && fake->Connect() && fake->LoadSolution("x.solw") && fake->IsConnected() && fake->IsSolutionLoaded());
    fake->PutGlobalVar("%A%", "Float", "9");
    fake->PutGlobalVar("B", "int", "3");
    const std::string tail = "Wrong type — App does not fix it, fix manually in VisionMaster: B (is int, need float)\r\nInvalid name: bad.name (contains forbidden chars? ; . / \\ ` { } [ ] < > ( ))";
    auto chk = fake->SyncGlobalVariables(specs, false);
    CHECK(!chk.Applied && !chk.Saved && chk.Items.size() == 4 && chk.Items[2].State == EGlobalVarState::Missing && fake->GlobalVarsSnapshot().size() == 2);
    CHECK_MSG(chk.ToText() == "Existing, correct type: 1 · Missing: 1 · Wrong type: 1 · Invalid name: 1\r\n" + tail + "\r\nStill missing: C", chk.ToText());
    auto app = fake->SyncGlobalVariables(specs, true);
    CHECK(app.Applied && app.Saved && app.Items[2].State == EGlobalVarState::Created && fake->SolutionSaveCount() == 1 && fake->GetGlobal("C") == 0.25);
    CHECK(fake->GlobalVarsSnapshot().at("%A%").Value == "9" && fake->GlobalVarsSnapshot().at("B").Type == "int");   // chỉ thêm, không sửa
    CHECK_MSG(app.ToText() == "Existing, correct type: 1 · Created: 1 · Missing: 0 · Wrong type: 1 · Invalid name: 1\r\nSolution saved.\r\n" + tail, app.ToText());
    auto again = fake->SyncGlobalVariables(specs, true);
    CHECK(again.Applied && !again.Saved && again.Count(EGlobalVarState::Existing) == 2 && fake->SolutionSaveCount() == 1);
    CHECK(fake->SaveSolution().empty() && fake->SolutionSaveCount() == 2);

    // ── FakeVision ──
    CHECK(!fake->LastImage() && fake->LastImageNozzle() == 0);
    CHECK(fake->ReadFlowOutputs("LocateCan").Error == "Simulated: no solution to read Output Params from");
    fake->SetFlowOutputSource([](const std::string& f) { VmFlowOutputs r; r.Flow = f; r.AddDeclared("ObjX_mm", 14); return r; });
    CHECK(fake->ReadFlowOutputs("LocateCan").Declared.size() == 1 && fake->ReadFlowOutputs("LocateCan").Declared[0].Numeric);

    std::string seenFlow;
    fake->SetAfterFlow([&](const std::string& f) { seenFlow = f; });
    VisionPose pose = fake->TriggerCalib(EVisionProcedure::GridCalib, AxisSnapshot{ 1, 2, 4 }, {});
    CHECK(pose.Found && pose.Error.empty() && fake->CalibPointCount() == 1 && fake->LastCalibAxes()->Z == 2 && fake->LastCalibAxes()->Nozzle == 4
          && seenFlow == "Grid calib" && fake->LastImageNozzle() == 4);
    fake->SetFoundRate(0);
    CHECK(fake->TriggerCalib(EVisionProcedure::GridCalib, {}, {}).Error == "Object not found" && fake->CalibPointCount() == 2);
    fake->SetFoundRate(1);

    CHECK(fake->CallFlow(Req("", {}, {}), {}).Error == "Missing Flow name" && fake->CallFlow(Req("Nope", {}, {}), {}).Error == "Flow \"Nope\" not found in solution");
    CHECK(fake->CallFlow(Req("LocateCan", { { "RunId", 1 } }, {}), {}).Error == "Global variable \"RunId\" not declared in solution" && fake->VmCallCount() == 0);
    fake->SeedVmSolution(catalog);
    CHECK(fake->CallFlow(Req("LocateCan", { { "", 1 } }, {}), {}).Error == "Empty global variable name");
    CHECK(fake->CallFlow(Req("LocateCan", { { "RunId", 5 }, { "X_mm", std::nan("") } }, {}), {}).Error == "Value for \"X_mm\" is not finite" && fake->GetGlobal("RunId") == 0);
    fake->SetGlobal("RunId", 0.1);
    CHECK(fake->GetGlobal("RunId") == 0.1 && std::isnan(fake->GetGlobal("none")) && fake->GlobalVarsSnapshot().at("RunId").Value == "0.1");

    // Biến rẽ nhánh: nạp điểm ghi 0 cùng vị trí trục; Flow soi mà biến còn 0 thì đi nhánh hiệu chuẩn — nạp thêm một điểm
    // rác, chỉ ra Found (đúng thứ App phải tránh bằng cách ghi 1 trước mỗi lần soi).
    CHECK(fake->GlobalVarsSnapshot().at("IsLocate").Type == "int" && fake->GetGlobal("IsLocate") == 1);
    int calib0 = fake->CalibPointCount();
    CHECK(fake->TriggerCalib(EVisionProcedure::GridCalib, AxisSnapshot{ 5, 6, 2 }, {}).Found && fake->GetGlobal("IsLocate") == 0
          && fake->GetGlobal("X_mm") == 5 && fake->GetGlobal("Z_mm") == 6 && fake->GetGlobal("Nozzle") == 2);
    auto wrongBranch = fake->CallFlow(Req("LocateCan", { { "RunId", 3 } }, { "Found" }, { "ObjFound" }), {});
    CHECK(wrongBranch.Ok && wrongBranch.MissingOptional == std::vector<std::string>{ "ObjFound" } && fake->CalibPointCount() == calib0 + 2
          && fake->LocateCount() == 0);
    fake->SetGlobal("IsLocate", 1);
    // Clear Point: đếm lần bấm, xoá bộ đếm điểm; lỗi thì không xoá gì.
    CHECK(fake->PointsSinceClear() > 0 && fake->ClearCalibPoints(EVisionProcedure::GridCalib).Ok() && fake->ClearPointCount() == 1
          && fake->PointsSinceClear() == 0);
    fake->SetFailClearPoint(true);
    auto clearFail = fake->ClearCalibPoints(EVisionProcedure::GridCalib);
    CHECK(!clearFail.Ok() && clearFail.Error.find("no N-point / Translation Calibration module") != std::string::npos && fake->ClearPointCount() == 1);
    fake->SetFailClearPoint(false);

    // Quang học giả: can trên nozzle 3 lệch (0.4, −0.2) — Obj = trục + lệch (nhiễu ±0.005).
    fake->SetNozzleDeviation(3, 0.4, -0.2);
    CHECK(fake->NozzleDeviation(3).first == 0.4 && fake->NozzleDeviation(0).first == 0 && fake->NozzleDeviation(8).second == 0);
    const std::vector<std::string> outs{ "EchoRunId", "EchoX_mm", "EchoZ_mm", "Found" };
    const std::vector<std::string> opt{ "ObjFound", "ObjX_mm", "ObjY_mm", "ObjRunId", "Ghost" };
    auto lc = fake->CallFlow(Req("LocateCan", { { "RunId", 77 }, { "X_mm", 3 }, { "Z_mm", 4 }, { "Nozzle", 3 } }, outs, opt), {});
    CHECK(lc.Ok && lc.GetInt("EchoRunId") == 77 && lc.Get("EchoX_mm") == 3 && lc.Get("EchoZ_mm") == 4 && lc.Get("ObjFound") == 1 && lc.Get("Found") == 1 && lc.GetInt("ObjRunId") == 77);
    CHECK_NEAR(lc.Get("ObjX_mm"), 3.4, 0.0061); CHECK_NEAR(lc.Get("ObjY_mm"), 3.8, 0.0061);
    CHECK(lc.MissingOptional == std::vector<std::string>{ "Ghost" } && fake->LocateCount() == 1 && seenFlow == "LocateCan" && fake->GetGlobal("X_mm") == 3
          && fake->LastImageNozzle() == 3);
    auto n1 = fake->CallFlow(Req("LocateCan", { { "RunId", 78 }, { "X_mm", 10 }, { "Z_mm", 0 }, { "Nozzle", 1 } }, outs, opt), {});
    CHECK_NEAR(n1.Get("ObjX_mm"), 10, 0.0061); CHECK_NEAR(n1.Get("ObjY_mm"), 0, 0.0061);   // nozzle 1 không lệch
    CHECK(fake->CallFlow(Req("LocateCan", {}, { "Ghost" }), {}).Error == "Flow \"LocateCan\" missing output \"Ghost\"" && fake->LocateCount() == 3);
    fake->SetFoundRate(0);
    auto lost = fake->CallFlow(Req("LocateCan", { { "RunId", 79 } }, outs, opt), {});
    CHECK(lost.Ok && lost.Get("Found") == 0 && lost.Get("ObjFound") == 0 && lost.Get("ObjX_mm") == 0);
    fake->SetFoundRate(1);
    fake->SetCorruptEchoOnce(true);
    auto forced = fake->CallFlow(Req("LocateCan", { { "RunId", 80 } }, outs, opt), {});
    CHECK(forced.GetInt("EchoRunId") == 81 && !fake->CorruptEchoOnce() && fake->CallFlow(Req("LocateCan", { { "RunId", 80 } }, outs, opt), {}).GetInt("EchoRunId") == 80);
    CHECK(fake->FlowHistorySnapshot().size() == (size_t)fake->VmCallCount() && fake->FlowHistorySnapshot().front() == "LocateCan");
    fake->ClearFlowHistory();
    CHECK(fake->FlowHistorySnapshot().empty());

    // ── VmCycle: hợp đồng, đọc kết quả, độ lệch, kiểm tra lưới, cổng (phép thuần) ──
    {
        VisionOutputNames n;
        CHECK(VmCycle::AxisVarProblem(nullptr) == "vision name config missing" && VmCycle::AxisVarProblem(&n).empty());
        VisionOutputNames bad = n; bad.GvAxisZ.clear();
        CHECK(VmCycle::AxisVarProblem(&bad) == "axis variable names (X, Z) empty on Setting page");
        auto g = VmCycle::AxisGlobals(nullptr, n, 42, 5);
        CHECK(g.size() == 4 && g[VmVars::RunId] == 42 && g[n.GvAxisX] == 0 && g[n.GvAxisZ] == 0 && g[n.GvNozzle] == 5);
        auto req = VmCycle::LocateRequest(n, g);
        CHECK(req.Flow == "LocateCan" && req.Camera == EVmCamera::Cam && req.Outputs.size() == 4 && req.OptionalOutputs.size() == 4 && req.Globals.at(n.GvIsLocate) == 1);
        CHECK(VmCycle::ExpectedFlowOutputs(n).size() == 1 && VmCycle::ExpectedFlowOutputs(n)[0].Names.size() == 8);
        CHECK(!VmCycle::FakeFlowOutputs("NoSuchFlow", n).Error.empty() && VmCycle::FakeFlowOutputs("LocateCan", n).Error.empty()
              && VmCycle::FakeFlowOutputs("LocateCan", n).Modules.size() == 1);
        CHECK(VisionConnect::NozzleOf(n, g) == 5 && VisionConnect::NozzleOf(n, {}) == 0 && VisionConnect::NozzleOf(n, { { "Nozzle", 9 } }) == 0);

        // Đọc kết quả: echo đúng, Rel = Obj − trục.
        g = VmCycle::AxisGlobals(nullptr, n, 77, 3);
        g[n.GvAxisX] = 3; g[n.GvAxisZ] = 4;
        auto read = VmCycle::ReadLocate(lc, g, n);
        CHECK(read.CallOk && read.EchoOk && read.Found && read.VmValid && read.AxX == 3 && read.AxZ == 4);
        CHECK_NEAR(read.RelX, 0.4, 0.0061); CHECK_NEAR(read.RelZ, -0.2, 0.0061);
        auto badEcho = VmCycle::ReadLocate(forced, g, n);
        CHECK(badEcho.CallOk && !badEcho.EchoOk && badEcho.EchoIssue.find("EchoRunId = 81 ≠ 77") != std::string::npos && !badEcho.VmValid);
        auto notFound = VmCycle::ReadLocate(lost, g, n);
        CHECK(notFound.CallOk && !notFound.Found && notFound.VmValid == notFound.EchoOk);
        auto failed = VmCycle::ReadLocate(VmCallResult::Fail("hỏng"), g, n);
        CHECK(!failed.CallOk && failed.CallError == "hỏng");

        // Độ lệch và đích bù: d = Rel − Ref; đích = điểm + Trim + Gain · d; vượt ngưỡng → NG.
        LaserCompSettings comp;   // GainX = GainZ = −1, MaxCompMm = 3
        NozzleCalib nc; nc.RefX = 0.1; nc.RefZ = -0.1; nc.TrimX = 0.05; nc.TrimZ = -0.02;
        auto d = VmCycle::Deviation(nc, 0.6, 0.3, comp, 400, 25);
        CHECK(d.Ok && d.Why.empty());
        CHECK_NEAR(d.Dx, 0.5, 1e-9); CHECK_NEAR(d.Dz, 0.4, 1e-9); CHECK_NEAR(d.TargetX, 399.55, 1e-9); CHECK_NEAR(d.TargetZ, 24.58, 1e-9);
        auto off = VmCycle::Deviation(nc, 0.6, 0.3, comp, 400, 25, false);   // vision tắt: chỉ Trim
        CHECK(off.Ok && off.Dx == 0 && off.Dz == 0 && off.TargetX == 400.05 && off.TargetZ == 24.98);
        comp.GainX = 1.0;
        CHECK_NEAR(VmCycle::Deviation(nc, 0.6, 0.3, comp, 400, 25).TargetX, 400.55, 1e-9);
        auto big = VmCycle::Deviation(nc, 3.5, 0, comp, 400, 25);
        CHECK(!big.Ok && big.Why.find("exceeds limit 3 mm") != std::string::npos && big.Why.find("X +3.400") != std::string::npos);
        CHECK(!VmCycle::Deviation(nc, std::nan(""), 0, comp, 400, 25).Ok);
        comp.MaxCompMm = 0;
        CHECK(VmCycle::Deviation(nc, 0, 0, comp, 400, 25).Why == "invalid MaxCompMm on Setting page");

        // Kiểm tra lưới: Rel đứng yên khi trục chạy → OK; Rel trôi theo trục → sai thang đo (8); lệch nhỏ → 4; trục kia chạy → 7.
        auto s0 = Cap(0.4, -0.2, 100, 20), s1 = Cap(0.4, -0.2, 103, 20), s2 = Cap(0.4, -0.2, 100, 23);
        auto j = VmCycle::JudgeGrid(s0, s1, s2, 0.05);
        CHECK(j.Status == VmStatus::Ok && j.StepX == 3 && j.StepZ == 3 && j.ErrX == 0 && j.CrossZ == 0);
        CHECK(VmCycle::JudgeGrid(s0, Cap(3.4, -0.2, 103, 20), s2, 0.05).Status == VmStatus::Code8);
        CHECK(VmCycle::JudgeGrid(s0, Cap(0.48, -0.2, 103, 20), s2, 0.05).Status == VmStatus::EccOut);
        CHECK(VmCycle::JudgeGrid(s0, Cap(0.4, -0.2, 103, 20.5), s2, 0.05).Status == VmStatus::AxisMoved);
        CHECK(VmCycle::JudgeGrid(s0, Cap(0.4, -0.2, 100.2, 20), s2, 0.05).Status == VmStatus::NoConclusion);
        CHECK(VmCycle::JudgeGrid(s0, s1, s2, std::nan("")).Status == VmStatus::Invalid);

        // Cổng hiệu chuẩn.
        ModelData model;
        SystemData sys;
        auto gate = VmCycle::EvaluateCalib(&model, &sys);
        CHECK(!gate.Ready() && gate.Block == "grid check not passed; nozzle 1, 2, 3, 4, 5, 6, 7 master not set" && std::isnan(gate.DaysSinceOldest));
        model.Calib.GridVerifiedAt = std::chrono::system_clock::now();
        for (int k = 1; k <= 7; ++k) model.Calib.Nozzle(k).RefAt = std::chrono::system_clock::now();
        gate = VmCycle::EvaluateCalib(&model, &sys);
        CHECK(gate.Ready() && gate.Describe() == "Calib: ready" && gate.DaysSinceOldest >= 0 && gate.DaysSinceOldest < 1);
        model.Calib.Nozzle(4).RefAt.reset();
        CHECK(VmCycle::EvaluateCalib(&model, &sys).Block == "nozzle 4 master not set" && VmCycle::EvaluateCalib(&model, &sys, 2).Ready());
        sys.LaserComp.MaxCompMm = 0;
        CHECK(VmCycle::EvaluateCalib(&model, &sys, 2).Block.find("invalid compensation limits") != std::string::npos);
        CHECK(VmCycle::EvaluateCalib(nullptr, &sys).Block == "model or config not loaded");
        CHECK(VmCycle::Fmt(1.5) == "1.5" && VmCycle::Fmt(-0.0001) == "0" && VmCycle::Fmt(std::nan("")) == "NaN (missing)");
    }

    fake->SetProcessTimeMs(3000);   // huỷ giữa lúc "chụp": ném OperationCanceled, không ghi biến
    {
        std::stop_source src;
        std::jthread killer([&] { base::Delay(60); src.request_stop(); });
        bool cancelled = false;
        base::Stopwatch sw;
        try { fake->CallFlow(Req("LocateCan", { { "RunId", 99 } }, {}), src.get_token()); }
        catch (const base::OperationCanceled&) { cancelled = true; }
        CHECK(cancelled && sw.ElapsedMilliseconds() < 1500 && fake->GetGlobal("RunId") == 80);
        std::stop_source src2; src2.request_stop();
        bool cancelled2 = false;
        try { fake->TriggerCalib(EVisionProcedure::GridCalib, {}, src2.get_token()); } catch (const base::OperationCanceled&) { cancelled2 = true; }
        CHECK(cancelled2);
    }
    fake->Disconnect();
    CHECK(!fake->IsConnected() && !fake->IsSolutionLoaded() && fake->CallFlow(Req("LocateCan", {}, {}), {}).Error == "Not connected");

    // ── VisionConnect: mô phỏng từ đầu tới cuối ──
    CHECK(VisionConnect::FlowNameOf(VisionOutputNames(), EVisionProcedure::GridCalib) == "LocateCan");
    auto cv = VisionConnect::CalibVars(VisionOutputNames(), AxisSnapshot{ 1, 2, 4 });
    CHECK(cv.size() == 4 && cv.at("IsLocate") == 0 && cv.at("X_mm") == 1 && cv.at("Z_mm") == 2 && cv.at("Nozzle") == 4);
    VisionOutputNames noZ; noZ.GvAxisZ.clear();
    auto av = VisionConnect::AxisVars(noZ, AxisSnapshot{ 1, 2, 4 });
    CHECK(av.size() == 2 && av.at("X_mm") == 1 && !av.count("Z_mm"));
    VisionConnect::SetRealDeviceFactory({});
    {
        auto sys = std::make_shared<SystemData>();
        sys->SimulationMode = true;
        sys->VisionOutputs.FlowLocateCan = "LC_custom";
        VisionConnect vc(sys);
        CHECK(vc.IsSimulation() && vc.Fake() && vc.Device() == vc.Fake() && !vc.IsConnected() && vc.CallFlow(Req("LC_custom", {}, {}), {}).Error == "Not connected");
        vc.Fake()->SetProcessTimeMs(5);
        CHECK(vc.Connect() && vc.IsConnected() && !vc.IsSolutionLoaded() && vc.LoadSolution("sim.solw") && vc.IsSolutionLoaded());
        auto r = vc.CallFlow(Req("LC_custom", { { "RunId", 5 }, { "X_mm", 1.25 } }, { "EchoRunId", "EchoX_mm", "ObjFound" }), {});
        CHECK(r.Ok && r.GetInt("EchoRunId") == 5 && r.Get("EchoX_mm") == 1.25 && vc.Fake()->GlobalVarsSnapshot().size() == 5);
        CHECK(vc.CallFlow(Req("LocateCan", {}, {}), {}).Error == "Flow \"LocateCan\" not found in solution");
        CHECK(vc.TriggerCalib(EVisionProcedure::GridCalib, AxisSnapshot{ 9, 0, 0 }, {}).Found && vc.Fake()->LastCalibAxes()->X == 9);
        auto sync = vc.SyncGlobalVariables(VisionGlobalVarCatalog::Build(sys.get()), false);
        CHECK(sync.Error.empty() && sync.Count(EGlobalVarState::Existing) == 5 && vc.SaveSolution().empty() && !vc.LastImage());
        CHECK(vc.ReadFlowOutputs("LC_custom").Error == "Simulated: no solution to read Output Params from");
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
        CHECK(vc.CallFlow(Req("LocateCan", {}, {}), {}).Error == "Not connected" && vc.TriggerCalib(EVisionProcedure::GridCalib, {}, {}).Error == "Not connected");
        CHECK(vc.SaveSolution() == "Solution not loaded" && vc.SyncGlobalVariables({}, false).Error == "Vision solution not loaded" && !vc.LastImage());
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
        CHECK(vc.Connect() && vc.IsConnected() && vc.Device() == stub && !vc.IsSolutionLoaded() && vc.CallFlow(Req("F", {}, {}), {}).Error == "Solution not loaded");
        stub->loadThrows = true;
        CHECK(!vc.LoadSolution("b.solw") && !AlarmManager::IsActive(AlarmCode::VisionLoadFailed));
        stub->loadThrows = false;
        CHECK(vc.LoadSolution("b.solw") && vc.IsSolutionLoaded());
        auto ok = vc.CallFlow(Req("F", { { "G", 4.5 } }, {}), {});
        CHECK(ok.Ok && ok.Get("Echo") == 4.5 && stub->calls == 1 && vc.LastImage() == stub->image && vc.LastImageNozzle() == 0);
        CHECK(vc.CallFlow(Req("", {}, {}), {}).Error == "Missing Flow name" && vc.ReadFlowOutputs("").Error == "Flow name not set on Setting page");
        auto fo = vc.ReadFlowOutputs("F");
        CHECK(fo.Error.empty() && fo.Declared.size() == 2 && fo.Declared[0].TypeText == "float" && fo.Declared[1].TypeText == "image" && !fo.Declared[1].Numeric);
        CHECK(vc.SyncGlobalVariables({}, true).Error == "sync hỏng" && vc.SaveSolution() == "đĩa đầy");
        CHECK(vc.TriggerCalib(EVisionProcedure::GridCalib, AxisSnapshot{ 1, 0, 1 }, {}).Found && !vc.TriggerCalib(EVisionProcedure::GridCalib, AxisSnapshot{ -1, 0, 1 }, {}).Found);
        // Clear Point qua VisionConnect: tới thiết bị, giữ khoá lệnh; tên Flow trống thì không gọi.
        auto cl = vc.ClearCalibPoints(EVisionProcedure::GridCalib);
        CHECK(cl.Ok() && cl.Modules.size() == 1 && stub->clearCalls == 1);
        sys->VisionOutputs.FlowLocateCan.clear();
        CHECK(vc.ClearCalibPoints(EVisionProcedure::GridCalib).Error == "Flow name not set on Setting page" && stub->clearCalls == 1);
        CHECK(vc.TriggerCalib(EVisionProcedure::GridCalib, AxisSnapshot{ 1, 0, 0 }, {}).Error == "Flow name not set on Setting page");
        sys->VisionOutputs.FlowLocateCan = "LocateCan";

        stub->throwOnCall = true;
        CHECK(vc.CallFlow(Req("F", {}, {}), {}).Error == "F: boom" && AlarmManager::ActiveCount() == 0);
        CHECK(vc.ClearCalibPoints(EVisionProcedure::GridCalib).Error == "boom");
        stub->throwOnCall = false;

        // Quá giờ: alarm 4003 dạng sự kiện; lần gọi xếp hàng sau một Run treo bị bỏ thì KHÔNG chạy muộn.
        stub->delayMs = 500;
        int before = stub->calls;
        base::Stopwatch sw;
        auto t1 = vc.CallFlow(Req("Slow", {}, {}), {});
        CHECK(!t1.Ok && t1.TimedOut && t1.Error == "Timeout waiting for Flow Slow" && sw.ElapsedMilliseconds() < 450);
        auto alarm = AlarmManager::Get(AlarmCode::VisionTriggerTimeout);
        CHECK(alarm && alarm->Detail == "Slow" && alarm->Source == "Vision" && !alarm->ConditionActive);
        auto t2 = vc.CallFlow(Req("Queued", {}, {}), {});
        CHECK(t2.TimedOut && t2.Error == "Timeout waiting for Flow Queued");
        base::Delay(600);
        CHECK(stub->calls == before + 1);
        AlarmManager::ForceClearAll();

        // Huỷ giữa lúc Flow chạy: không phải quá giờ, không alarm.
        {
            std::stop_source src;
            std::jthread killer([&] { base::Delay(40); src.request_stop(); });
            auto c = vc.CallFlow(Req("Cut", {}, {}), src.get_token());
            CHECK(!c.Ok && !c.TimedOut && c.Error == "Canceled while running Flow Cut" && AlarmManager::ActiveCount() == 0);
        }
        base::Delay(600);
        auto tc = vc.TriggerCalib(EVisionProcedure::GridCalib, AxisSnapshot{ 1, 0, 2 }, {});
        CHECK(!tc.Found && tc.Error == "Timeout" && AlarmManager::Get(AlarmCode::VisionTriggerTimeout) && AlarmManager::Get(AlarmCode::VisionTriggerTimeout)->Detail == "LocateCan");
        std::stop_source dead; dead.request_stop();
        CHECK(vc.TriggerCalib(EVisionProcedure::GridCalib, AxisSnapshot{ 1, 0, 0 }, dead.get_token()).Error == "Canceled");
        base::Delay(600);
        stub->delayMs = 0;
        CHECK(vc.CallFlow(Req("F", { { "G", 2 } }, {}), {}).Get("Echo") == 2);
        vc.Disconnect();
        CHECK(!vc.IsConnected() && !stub->connected && vc.CallFlow(Req("F", {}, {}), {}).Error == "Not connected");
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
