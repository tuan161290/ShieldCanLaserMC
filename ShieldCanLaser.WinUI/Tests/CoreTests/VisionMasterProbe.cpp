// Chạy thật VisionMasterDevice trên máy có cài VisionMaster 4.4 — KHÔNG nằm trong lượt test mặc định (máy không
// có VisionMaster không chạy được). Gọi bằng `CoreTests.exe vmprobe`.
//
// Trên máy dev hiện tại LoadSolution trả 0xE0000000 (IMVS_EC_VERSION) với mọi file (bàn giao VM-WinUI3 §10), nên
// probe chỉ kiểm: tạo/huỷ instance, phiên bản SDK, nạp solution hỏng được báo sạch (không sập, có mã lỗi), mọi
// thao tác khi chưa nạp solution trả thất bại sạch, Disconnect hai lần, và phép đổi ảnh SDK → ImageData.
#include "TestKit.h"

#include "Base/Async.h"
#include "Base/Text.h"
#include "Communication/VisionConnect.h"
#include "SysConfig/AppPaths.h"
#include "Utility/Log/LogWorker.h"
#include "Vision/VisionMasterDevice.h"
#include "Vision/VmImageConvert.h"

#include <algorithm>
#include <cstdio>
#include <stop_token>
#include <filesystem>
#include <format>

using namespace scs;
namespace fs = std::filesystem;

namespace
{
    bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    void ImageConversion()
    {
        // Mono8 2x2 → Gray8, chép nguyên.
        const unsigned char mono[4] = { 1, 2, 3, 4 };
        auto g = VmImageToImageData(mono, sizeof(mono), 2, 2, VmPixelFormat::Mono8);
        CHECK(g && g->Width == 2 && g->Height == 2 && g->Channels == 1 && g->Stride == 2);
        CHECK(g && g->Pixels == std::vector<uint8_t>({ 1, 2, 3, 4 }) && g->Pixels.data() != mono);

        // RGB24 xen kẽ 2x1 → BGR24.
        const unsigned char rgb[6] = { 10, 20, 30, 40, 50, 60 };
        auto c = VmImageToImageData(rgb, sizeof(rgb), 2, 1, VmPixelFormat::Rgb24Packed);
        CHECK(c && c->Channels == 3 && c->Stride == 6);
        CHECK(c && c->Pixels == std::vector<uint8_t>({ 30, 20, 10, 60, 50, 40 }));

        // RGB24 ba mặt phẳng 2x1 (RR GG BB) → BGR24.
        const unsigned char planar[6] = { 10, 40, 20, 50, 30, 60 };
        auto p = VmImageToImageData(planar, sizeof(planar), 2, 1, VmPixelFormat::Rgb24Planar);
        CHECK(p && p->Pixels == std::vector<uint8_t>({ 30, 20, 10, 60, 50, 40 }));

        // Dữ liệu dư (DataLen lớn hơn cần) vẫn nhận, chỉ chép phần ảnh.
        const unsigned char big[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
        auto b = VmImageToImageData(big, sizeof(big), 2, 2, VmPixelFormat::Mono8);
        CHECK(b && b->Pixels.size() == 4);

        std::string why;
        CHECK(!VmImageToImageData(nullptr, 4, 2, 2, VmPixelFormat::Mono8, &why) && !why.empty());
        why.clear();
        CHECK(!VmImageToImageData(mono, 3, 2, 2, VmPixelFormat::Mono8, &why) && !why.empty());
        why.clear();
        CHECK(!VmImageToImageData(mono, 4, 0, 2, VmPixelFormat::Mono8, &why) && !why.empty());
        why.clear();
        CHECK(!VmImageToImageData(mono, 4, 2, 2, 0x02180015u, &why) && Contains(why, "0x02180015"));
    }

    /// Mọi thao tác khi chưa nạp solution: thất bại sạch, không ném.
    void NotLoaded(VisionMasterDevice& d)
    {
        CHECK(!d.IsSolutionLoaded());
        VisionPose pose;
        CHECK_MSG((pose = d.TriggerCalib(EVisionProcedure::GridCalib, AxisSnapshot{}, {}), !pose.Found && pose.Error == "Solution not loaded"), pose.Error);
        VmCallRequest req;
        req.Flow = "LocateCan";
        req.Globals["X_mm"] = 1.0;
        req.Outputs = { "Found" };
        auto r = d.CallFlow(req, {});
        CHECK_MSG(!r.Ok && r.Error == "Solution not loaded", r.Error);
        req.Flow.clear();
        r = d.CallFlow(req, {});
        CHECK_MSG(!r.Ok && r.Error == "Missing Flow name", r.Error);
        auto fo = d.ReadFlowOutputs("LocateCan");
        CHECK_MSG(fo.Error == "Solution not loaded" && fo.Declared.empty(), fo.Error);
        auto rep = d.SyncGlobalVariables(VisionGlobalVarCatalog::Build(static_cast<const SystemData*>(nullptr)), false);
        CHECK_MSG(rep.Error == "Vision solution not loaded", rep.Error);
        CHECK(d.SaveSolution() == "Solution not loaded");
        CHECK(d.RenderSource("LocateCan") == nullptr && d.RenderSource("LocateCan.Echo0") == nullptr && d.RenderSource("") == nullptr);
        CHECK(d.FlowNames().empty());
        CHECK(!d.LastImage() && d.LastImageNozzle() == 0);
    }

    /// Nạp một file: phải ném VisionDeviceError mang mã lỗi SDK trong cả Code lẫn thông báo. Trả mã lỗi.
    int LoadMustFail(VisionMasterDevice& d, const std::string& path)
    {
        int code = 0;
        std::string what;
        bool threwRight = false;
        try { d.LoadSolution(path); }
        catch (const VisionDeviceError& e) { threwRight = true; code = e.Code; what = e.what(); }
        catch (const std::exception& e) { what = std::string("exception khác: ") + e.what(); }
        catch (...) { what = "exception lạ"; }
        std::printf("    LoadSolution(%s) -> code 0x%08X, \"%s\"\n", path.c_str(), static_cast<unsigned>(code), what.c_str());
        CHECK_MSG(threwRight && code != 0, what);
        CHECK_MSG(Contains(what, std::format("0x{:08X}", static_cast<unsigned>(code))), what);
        CHECK(!d.IsSolutionLoaded());
        return code;
    }

    /// Gọi f; phải ném VisionDeviceError có chứa `part`. In thông báo để báo cáo.
    template <class F>
    void MustThrow(const char* label, F&& f, const std::string& part)
    {
        std::string what = "(không ném)";
        bool right = false;
        try { f(); }
        catch (const VisionDeviceError& e) { what = e.what(); right = Contains(what, part); }
        catch (const std::exception& e) { what = std::string("exception khác: ") + e.what(); }
        catch (...) { what = "exception lạ"; }
        std::printf("    %s -> \"%s\"\n", label, what.c_str());
        CHECK_MSG(right, std::string(label) + ": " + what);
    }

    /// Các đường SAU khi nạp — trên bản sao solution mẫu của hãng (Flow1 → Image Source1 → Circle Search1,
    /// output Flow "ImageData" theo GetResultControlDlg.cpp). Không có module Global Variable.
    void Loaded(VisionMasterDevice& d)
    {
        CHECK(d.IsSolutionLoaded());
        auto flows = d.FlowNames();
        std::string list;
        for (auto& f : flows) list += f + ";";
        std::printf("    FlowNames = %s\n", list.c_str());
        CHECK(std::find(flows.begin(), flows.end(), "Flow1") != flows.end());

        void* whole = d.RenderSource("Flow1");
        void* circle = d.RenderSource("Flow1.Circle Search1");
        std::printf("    RenderSource Flow1=%p  Flow1.Circle Search1=%p\n", whole, circle);
        CHECK(whole != nullptr && circle != nullptr);
        CHECK(d.RenderSource("Nope") == nullptr && d.RenderSource("Flow1.Nope") == nullptr);

        VmFlowOutputs fo;
        try { fo = d.ReadFlowOutputs("Flow1"); } catch (const std::exception& e) { fo.Error = e.what(); }
        std::printf("    ReadFlowOutputs(Flow1): error=\"%s\" inputs=%d declared=", fo.Error.c_str(), fo.InputParamCount);
        for (auto& o : fo.Declared) std::printf("%s(%d,%s) ", o.Name.c_str(), o.TypeCode, o.TypeText.c_str());
        std::printf("\n");
        CHECK(fo.Error.empty() && fo.Flow == "Flow1");
        MustThrow("ReadFlowOutputs(Nope)", [&] { d.ReadFlowOutputs("Nope"); }, "Flow \"Nope\" not found");
        MustThrow("ReadFlowOutputs(Flow1.Circle Search1)", [&] { d.ReadFlowOutputs("Flow1.Circle Search1"); }, "not found");

        VmCallRequest req;
        req.Flow = "Flow1";
        req.Camera = EVmCamera::Cam;
        req.Globals["Nozzle"] = 4;
        req.OptionalOutputs = { "NoSuchOut" };
        VmCallResult r;
        try { r = d.CallFlow(req, {}); } catch (const std::exception& e) { r.Error = e.what(); }
        auto img = d.LastImage();
        std::printf("    CallFlow(Flow1): ok=%d error=\"%s\" missingOptional=%zu image=%s\n", r.Ok, r.Error.c_str(), r.MissingOptional.size(),
                    img ? std::format("{}x{}x{} stride {} bytes {}", img->Width, img->Height, img->Channels, img->Stride, img->Pixels.size()).c_str() : "null");
        // Solution mẫu không có module Global Variable → ghi biến Nozzle phải ném; bỏ biến đi thì chạy được.
        if (!r.Ok)
        {
            CHECK_MSG(Contains(r.Error, "Global Variable"), r.Error);
            req.Globals.clear();
            try { r = d.CallFlow(req, {}); } catch (const std::exception& e) { r.Error = e.what(); }
            img = d.LastImage();
        }
        CHECK(r.Ok && r.MissingOptional == std::vector<std::string>({ "NoSuchOut" }));
        // Solution mẫu trên máy dev KHÔNG khai output "ImageData" ở cấp Flow (GetAllOutputNameInfo n = 0,
        // GetOutputImageV2 ném 0xE000001F, Image Source không có ảnh) — nên ảnh null là đúng. Có ảnh thì phải hợp lệ.
        CHECK(!img || (img->Width > 0 && img->Height > 0 && img->Pixels.size() == static_cast<size_t>(img->Stride) * img->Height));

        req.Camera = EVmCamera::None;
        req.Outputs = { "NoSuchOut" };
        MustThrow("CallFlow(required NoSuchOut)", [&] { d.CallFlow(req, {}); }, "missing output \"NoSuchOut\"");
        req.Outputs.clear();
        req.Globals["X_mm"] = 1.0;
        MustThrow("CallFlow(Globals X_mm)", [&] { d.CallFlow(req, {}); }, "Global Variable");
        req.Globals.clear();
        req.Flow = "Nope";
        MustThrow("CallFlow(Nope)", [&] { d.CallFlow(req, {}); }, "Flow \"Nope\" not found");
        std::stop_source cancelled;
        cancelled.request_stop();
        req.Flow = "Flow1";
        bool cancelledOk = false;
        try { d.CallFlow(req, cancelled.get_token()); } catch (const base::OperationCanceled&) { cancelledOk = true; } catch (...) {}
        CHECK(cancelledOk);

        MustThrow("TriggerCalib(GridCalib)", [&] { d.TriggerCalib(EVisionProcedure::GridCalib, AxisSnapshot{}, {}); }, "Global Variable");

        auto rep = d.SyncGlobalVariables(VisionGlobalVarCatalog::Build(static_cast<const SystemData*>(nullptr)), true);
        std::printf("    SyncGlobalVariables(apply) error=\"%s\"\n", rep.Error.c_str());
        CHECK(Contains(rep.Error, "Global Variable") && !rep.Saved);

        std::string saved = d.SaveSolution();
        std::printf("    SaveSolution -> \"%s\"\n", saved.c_str());
        CHECK(saved.empty());
    }
}

int RunVisionMasterProbe()
{
    testkit::Group group("VisionMasterProbe");

    fs::path root = fs::temp_directory_path() / ("scs_vmprobe_" + base::NowString("yyyyMMdd_HHmmssfff"));
    AppPaths::Init(base::PathToU8(root));
    LogWorker::Instance().Init(AppPaths::Root());
    CHECK(AppPaths::Root() == root);

    ImageConversion();

    auto sys = std::make_shared<SystemData>();
    sys->SimulationMode = false;
    auto getSys = [sys] { return sys; };
    const std::string missing = "C:\\no\\such\\dir\\none.solw";
    const fs::path sample = L"C:\\Program Files\\VisionMaster4.4.0\\Development\\V4.x\\Samples\\VC\\PlatformSDKSampleVC\\GetResultControl\\GetResultSample.solw";
    fs::create_directories(root);
    std::error_code ec;
    fs::copy_file(sample, root / "GetResultSample.solw", fs::copy_options::overwrite_existing, ec);
    CHECK_MSG(!ec, ec.message());
    const std::string copy = base::PathToU8(root / "GetResultSample.solw");

    {
        VisionMasterDevice d(getSys);
        std::string ver = d.SdkVersion();
        std::printf("    SdkVersion = %s\n", ver.c_str());
        CHECK(Contains(ver, "V4.4"));

        // Trước Connect.
        CHECK(!d.IsConnected());
        bool threw = false;
        try { d.LoadSolution(missing); } catch (const VisionDeviceError& e) { threw = Contains(e.what(), "Connect"); }
        CHECK(threw);
        NotLoaded(d);

        for (int round = 0; round < 3; ++round)
        {
            bool ok = false;
            std::string err;
            try { ok = d.Connect(); } catch (const std::exception& e) { err = e.what(); }
            CHECK_MSG(ok && d.IsConnected(), err);
            CHECK(d.Connect());   // gọi lại khi đã kết nối: dùng lại instance
            NotLoaded(d);
            CHECK(LoadMustFail(d, missing) == static_cast<int>(0xE0000203));   // IMVS_EC_FILE_NOT_FOUND

            // Bản sao solution mẫu (SaveSolution ghi vào chính file đang nạp — không bao giờ ghi vào C:\Program Files).
            // Máy chưa nạp được solution (bàn giao §10, 0xE0000000) thì chỉ đòi lỗi được báo sạch.
            bool loaded = false;
            std::string what;
            int code = 0;
            try { loaded = d.LoadSolution(copy); }
            catch (const VisionDeviceError& e) { what = e.what(); code = e.Code; }
            catch (const std::exception& e) { what = std::string("exception khác: ") + e.what(); }
            std::printf("    round %d: LoadSolution(copy of GetResultSample.solw) -> %s\n", round, loaded ? "OK" : what.c_str());
            if (loaded) Loaded(d);
            else CHECK_MSG(code != 0 && Contains(what, std::format("0x{:08X}", static_cast<unsigned>(code))), what);
            d.Disconnect();
            CHECK(!d.IsSolutionLoaded());
            NotLoaded(d);
            d.Disconnect();
            CHECK(!d.IsConnected() && !d.IsSolutionLoaded());
        }

        // Hai thiết bị nối tiếp trong cùng tiến trình (hàm tạo của VisionConnect có thể được gọi lại).
        auto other = VisionMasterDevice::Create(getSys);
        CHECK(other && other->Connect());
        other->Disconnect();
    }

    // Qua lớp bọc VisionConnect như app thật dùng.
    {
        VisionConnect::SetRealDeviceFactory([getSys] { return VisionMasterDevice::Create(getSys); });
        VisionConnect vc(sys);
        CHECK(!vc.IsSimulation() && vc.Connect() && vc.IsConnected());
        CHECK(!vc.LoadSolution(missing) && !vc.IsSolutionLoaded());
        bool loaded = vc.LoadSolution(copy);
        auto r = vc.CallFlow(VmCallRequest{ "LocateCan" }, {});
        std::printf("    VisionConnect: load=%d CallFlow(LocateCan) -> \"%s\"\n", loaded, r.Error.c_str());
        CHECK_MSG(!r.Ok && (loaded ? Contains(r.Error, "Flow \"LocateCan\" not found") : r.Error == "Solution not loaded"), r.Error);
        AxisSnapshot n2{};
        n2.Nozzle = 2;
        auto pose = vc.TriggerCalib(EVisionProcedure::GridCalib, n2, {});
        std::printf("    VisionConnect: TriggerCalib(GridCalib nozzle 2, LocateCan nhánh hiệu chuẩn) -> \"%s\"\n", pose.Error.c_str());
        CHECK(!pose.Found && !pose.Error.empty());
        vc.Disconnect();
        vc.Disconnect();
        VisionConnect::SetRealDeviceFactory({});
    }

    // DisposeResource: một lần, sau đó không kết nối lại được nữa — báo lỗi sạch.
    VisionMasterDevice::DisposeSdk();
    VisionMasterDevice::DisposeSdk();
    {
        VisionMasterDevice d(getSys);
        bool threw = false;
        try { d.Connect(); } catch (const VisionDeviceError& e) { threw = Contains(e.what(), "DisposeSdk"); }
        CHECK(threw && !d.IsConnected());
        NotLoaded(d);
    }

    return group.Result();
}
