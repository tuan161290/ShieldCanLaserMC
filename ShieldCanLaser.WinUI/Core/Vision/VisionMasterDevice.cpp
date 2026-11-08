#include "Vision/VisionMasterDevice.h"

#include "Base/Async.h"
#include "Base/Text.h"
#include "Communication/VisionConnect.h"
#include "Vision/VmImageConvert.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>

#include <windows.h>

// Header của SDK dùng bảng mã tiếng Trung (C4819) và tự `using namespace std` ở phạm vi toàn cục — chỉ được
// nằm trong file .cpp này, không bao giờ lọt ra header công khai.
#pragma warning(push)
#pragma warning(disable: 4819 4996 4251 4275)
#include "IVmSolution.h"
#include "IVmProcedure.h"
#include "IVmGroup.h"
#include "VMException.h"
#include "iMVS-6000PlatformSDKC.h"
#pragma warning(pop)

// LƯU Ý BUILD: mọi hàm xuất của SDK khai `extern "C"` nhưng vẫn ném CVmException. Phải biên dịch với /EHs
// (không phải /EHsc), nếu không trình biên dịch bỏ luôn các khối catch dưới đây và tiến trình — đang chạy
// cả EtherCAT master — chết với 0xC0000409 (bàn giao VM-WinUI3 §7).

namespace scs
{
    namespace vm = VisionMasterSDK;

    namespace
    {
        using Solution = vm::VmSolution::IVmSolution;
        using Procedure = vm::VmProcedure::IVmProcedure;
        using ProcResults = vm::VmProcedure::IMVSProcedureResults;
        using GroupTool = vm::IMVSGroup::IMVSGroupTool;
        using GroupResults = vm::IMVSGroup::IMVSGroupResults;

        constexpr int CodeProcedureNotFound = static_cast<int>(0xE0000316);   // IMVS_EC_MODULE_PROCEDURE_NOT_FOUND
        constexpr int CodeModuleNotFound = static_cast<int>(0xE0000302);      // IMVS_EC_MODULE_NOT_FOUND
        constexpr int CodeInvalidHandle = static_cast<int>(0xE0000006);       // IMVS_EC_INVALID_HANDLE

        /// Chờ callback "Flow xong" tối đa bấy nhiêu sau khi Run() (lệnh chặn) đã trả về. Đây KHÔNG phải canh
        /// giờ trigger (việc của VisionConnect) — chỉ là chặn trên cho bước "đợi SDK đẩy xong kết quả".
        constexpr auto RunEndGrace = std::chrono::milliseconds(1000);

        /// Hai hàm cho UI chỉ thử khoá SDK chừng này rồi bỏ.
        constexpr auto UiLockWait = std::chrono::milliseconds(30);

        /// DisposeResource() đã chạy — SDK không dùng lại được trong tiến trình này nữa.
        std::atomic<bool> g_sdkDisposed{ false };

        std::string Hex(int code) { return std::format("0x{:08X}", static_cast<unsigned>(code)); }

        std::string Describe(const std::string& what, const vm::CVmException& e)
        {
            const char* d = e.GetDescription();
            std::string text = (d && *d) ? d : "VisionMaster SDK error";
            return what + ": " + text + " (" + Hex(e.GetErrorCode()) + ")";
        }

        /// Bọc MỘT lời gọi SDK: CVmException → VisionDeviceError kèm mã lỗi; exception lạ → VisionDeviceError.
        template <class F>
        auto Sdk(const char* what, F&& f) -> decltype(f())
        {
            try { return f(); }
            catch (const vm::CVmException& e) { throw VisionDeviceError(Describe(what, e), e.GetErrorCode()); }
            catch (const std::exception&) { throw; }
            catch (...) { throw VisionDeviceError(std::string(what) + ": VisionMaster SDK threw unknown error", -2); }
        }

        template <std::size_t N>
        std::string Str(const char (&a)[N]) { return std::string(a, strnlen(a, N)); }

        bool EnsureSdkDll()
        {
            static const bool ok = GetModuleHandleW(L"iMVS-6000PlatformSDK.dll") != nullptr
                                   || LoadLibraryW(L"iMVS-6000PlatformSDK.dll") != nullptr;
            return ok;
        }

        void RequireSdkDll()
        {
            if (!EnsureSdkDll())
                throw VisionDeviceError("Failed to load iMVS-6000PlatformSDK.dll — VisionMaster 4.4 not installed "
                                        "or PATH missing Applications\\PublicFile\\x64");
        }

        struct RunSignal
        {
            std::mutex mutex;
            std::condition_variable_any cv;
            bool done{ false };
            bool filter{ false };
            unsigned processId{ 0 };
            bool everSeen{ false };
        };

        /// Callback của SDK — đến từ luồng của SDK. Chỉ đặt cờ, không gọi lại SDK, không để lọt exception.
        /// Khuôn lấy từ Samples\VC\...\GetResultControl\GetResultControlDlg.cpp (CallBackModuRes) và
        /// VmWinUiProbe\VmSdk.cpp: nInfoType = WORK_STATE, nWorkStatus = 0 nghĩa là Flow đã chạy xong.
        int __stdcall OnSdkCallback(OUT ::OutputPlatformInfo* const info, IN void* const user)
        {
            try
            {
                auto* sig = static_cast<RunSignal*>(user);
                if (!sig || !info || !info->pData) return IMVS_EC_OK;
                if (info->nInfoType != IMVS_ENUM_CTRLC_OUTPUT_PLATFORM_INFO_WORK_STATE) return IMVS_EC_OK;
                auto* ws = static_cast<IMVS_PF_MODULE_WORK_STAUS*>(info->pData);
                if (ws->nWorkStatus != 0) return IMVS_EC_OK;
                {
                    std::lock_guard<std::mutex> lock(sig->mutex);
                    sig->everSeen = true;
                    if (!sig->filter || ws->nProcessID == sig->processId) sig->done = true;
                }
                sig->cv.notify_all();
            }
            catch (...) {}
            return IMVS_EC_OK;
        }

        /// Đọc một output số, chấp nhận CẢ HAI kiểu: thử GetOutputFloat trước, không có thì GetOutputInt.
        /// Vì sao cần cả hai: nhiều kết quả tự nhiên của VisionMaster là Int (Module Status, Match Num,
        /// CalibPointNum) và giao diện khai output của Flow lọc danh sách Subscribe theo kiểu. Output sai
        /// kiểu làm GetOutputFloat trả rỗng y như output không tồn tại — Tuấn dính lỗi này 2026-09-12.
        /// Chỉ lấy phần tử [0]. Dùng cho cả Flow lẫn Group — hai lớp kết quả có cùng hai hàm đọc.
        /// SDK: IMVSProcedureResults / IMVSGroupResults ::GetOutputFloat / GetOutputInt (IVmProcedure.h, IVmGroup.h) —
        /// chưa chạy được ở máy dev.
        template <class Results>
        bool TryReadNumber(Results* res, const std::string& name, float& value)
        {
            value = 0.0f;
            if (!res) return false;
            try
            {
                vm::FloatDataArray f = res->GetOutputFloat(name.c_str());
                if (f.nValueNum >= 1 && f.pFloatVal != nullptr) { value = f.pFloatVal[0]; return true; }
            }
            catch (...) { /* output không phải kiểu float — thử kiểu int bên dưới */ }
            try
            {
                vm::IntDataArray n = res->GetOutputInt(name.c_str());
                if (n.nValueNum >= 1 && n.pIntVal != nullptr) { value = static_cast<float>(n.pIntVal[0]); return true; }
            }
            catch (...) { /* không đọc được kiểu nào — coi như thiếu output */ }
            return false;
        }

        /// Ảnh chỉ để hiển thị, không phải số dùng để phán OK/NG — lỗi thì im lặng trả nullptr.
        /// SDK: IMVSProcedureResults::GetOutputImageV2 → IoImage.stImage (mẫu GetResultControlDlg.cpp dòng
        /// "GetOutputImageV2("ImageData")") — chưa chạy được ở máy dev.
        std::shared_ptr<const ImageData> TryReadImage(ProcResults* res, const std::string& name)
        {
            if (!res) return nullptr;
            try
            {
                vm::IoImage io = res->GetOutputImageV2(name.c_str());
                const vm::ImageBaseData& b = io.stImage;
                return VmImageToImageData(b.ImageData, b.DataLen, b.Width, b.Height, static_cast<unsigned>(b.Pixelformat));
            }
            catch (...) { return nullptr; }
        }
    }

    struct VisionMasterDevice::Impl
    {
        std::function<std::shared_ptr<SystemData>()> sys;

        /// Xếp hàng mọi lần chạm SDK (xem chú thích luồng ở header). Re-entrant như lock của C#.
        std::recursive_timed_mutex sdk;
        Solution* sol{ nullptr };
        std::atomic<bool> connected{ false };
        std::atomic<bool> loaded{ false };
        std::shared_ptr<RunSignal> signal = std::make_shared<RunSignal>();
        int runsWithoutCallback{ 0 };
        /// Giữ đối tượng tham số của module đang hiển thị sống tới lần RenderSource kế tiếp.
        std::unique_ptr<vm::CModuParamsBase> renderParams;

        mutable std::mutex cacheMutex;
        std::vector<std::string> flowNames;
        std::shared_ptr<const ImageData> image;
        int imageNozzle{ 0 };

        VisionOutputNames Names() const
        {
            std::shared_ptr<SystemData> s;
            try { if (sys) s = sys(); } catch (...) {}
            return s ? s->VisionOutputs : VisionOutputNames{};
        }

        /// Cất ảnh CAM mới nhất kèm nozzle của lần gọi (một camera cho cả 7 nozzle).
        void SetImage(std::shared_ptr<const ImageData> img, int nozzle)
        {
            std::lock_guard<std::mutex> l(cacheMutex);
            image = std::move(img);
            imageNozzle = nozzle;
        }

        // ───────────────────────── Flow ─────────────────────────

        /// Tên → (có trong solution, ID). SDK: IVmSolution::GetAllProcedureList (IVmSolution.h). Trả false khi
        /// SDK không đưa được danh sách.
        bool ListFlows(std::vector<std::pair<std::string, unsigned>>& out)
        {
            out.clear();
            try
            {
                ::ProcessInfoList* list = sol->GetAllProcedureList();
                if (!list) return false;
                for (unsigned i = 0; i < list->nNum && i < std::size(list->astProcessInfo); ++i)
                    out.emplace_back(Str(list->astProcessInfo[i].strProcessName), list->astProcessInfo[i].nProcessID);
                return true;
            }
            catch (...) { return false; }
        }

        void RefreshFlowNames()
        {
            std::vector<std::pair<std::string, unsigned>> flows;
            if (!ListFlows(flows)) return;
            std::vector<std::string> names;
            for (auto& f : flows) names.push_back(f.first);
            std::lock_guard<std::mutex> l(cacheMutex);
            flowNames = std::move(names);
        }

        /// Lấy Flow theo tên. operator[] của SDK trả void* cho CẢ Flow lẫn module ("Flow.Module") nên phải đối
        /// chiếu với danh sách Flow trước khi ép kiểu — bản .NET có `as VmProcedure` làm việc đó.
        /// SDK: IVmSolution::operator[] (mẫu GetResultControlDlg.cpp: (IVmProcedure*)(*m_pVmSol)["Flow1"]).
        Procedure* GetProcedure(const std::string& flow, bool& hasId, unsigned& id)
        {
            auto missing = [&flow](int code = -1)
            {
                return VisionDeviceError("Flow \"" + flow + "\" not found in solution — check name in VisionMaster IDE", code);
            };
            hasId = false;
            id = 0;
            if (flow.empty()) throw missing();

            std::vector<std::pair<std::string, unsigned>> flows;
            if (ListFlows(flows))
            {
                for (auto& f : flows)
                    if (f.first == flow) { hasId = true; id = f.second; break; }
                if (!hasId) throw missing();
            }
            else if (flow.find('.') != std::string::npos) throw missing();   // không kiểm được thì không ép kiểu một đường dẫn module

            void* p = nullptr;
            try { p = (*sol)[flow.c_str()]; }
            catch (const vm::CVmException& e)
            {
                if (e.GetErrorCode() == CodeProcedureNotFound || e.GetErrorCode() == CodeModuleNotFound) throw missing(e.GetErrorCode());
                throw VisionDeviceError(Describe("Find Flow \"" + flow + "\"", e), e.GetErrorCode());
            }
            catch (...) { throw VisionDeviceError("Find Flow \"" + flow + "\": VisionMaster SDK threw unknown error", -2); }
            if (!p) throw missing();
            return static_cast<Procedure*>(p);
        }

        /// Chạy Flow MỘT lần (chạy hai lần là kích camera thật hai lần cho một lần trigger logic) rồi trả đối
        /// tượng kết quả của đúng lần chạy đó.
        ///
        /// <c>EnableResultCallback()</c> khi cần ảnh — không gọi thì GetOutputImageV2 không có dữ liệu, dù
        /// output số vẫn đọc được (mẫu GetResultControl). <c>Run()</c> mặc định bIsWait = true nên CHẶN tới
        /// khi Flow xong (IVmProcedure.h) — như VmProcedure.Run() bản .NET; VisionConnect canh giờ bên ngoài.
        /// Mẫu VC của hãng còn chờ callback WORK_STATE rồi mới đọc kết quả, nên sau Run() ta chờ thêm callback
        /// đó theo kiểu sự kiện, nghe token huỷ, tối đa RunEndGrace; nếu một lần chạy trước đã hết khoảng chờ
        /// mà callback chưa từng tới (cấu hình SDK không phát) thì thôi không chờ nữa.
        /// Chưa chạy được ở máy dev (không nạp được solution).
        ProcResults* RunFlow(Procedure* proc, bool hasId, unsigned id, bool wantImage, const std::stop_token& ct)
        {
            base::ThrowIfCancelled(ct);
            if (wantImage) Sdk("Enable Flow result callback", [&] { proc->EnableResultCallback(); });

            {
                std::lock_guard<std::mutex> lock(signal->mutex);
                signal->done = false;
                signal->filter = hasId;
                signal->processId = id;
            }

            Sdk("Run Flow", [&] { proc->Run(); });

            {
                std::unique_lock<std::mutex> lock(signal->mutex);
                bool skip = !signal->everSeen && runsWithoutCallback > 0;
                if (!signal->done && !skip)
                    signal->cv.wait_for(lock, ct, RunEndGrace, [this] { return signal->done; });
                if (!signal->done) ++runsWithoutCallback;
            }
            base::ThrowIfCancelled(ct);

            return Sdk("Get Flow result", [&] { return proc->GetResult(); });
        }

        // ───────────────────────── Group kiểm tra ─────────────────────────

        static bool IsGroupType(const std::string& type) { return type.rfind("IMVSGroup", 0) == 0; }

        /// Rỗng = tên cuối của <paramref name="path"/> ("Flow.Group", "Flow.Group1.Group2") là một module loại Group trong
        /// danh sách module của Flow (GetAllModuleList trả cả module nằm trong Group); khác rỗng = lý do. Loại module đọc từ
        /// strModuleName (tên lớp của SDK), không dựa vào tên hiển thị.
        std::string FindGroup(Procedure* proc, const std::string& flow, const std::string& path)
        {
            if (path.size() <= flow.size() + 1 || path.compare(0, flow.size() + 1, flow + ".") != 0)
                return "Path \"" + path + "\" is not inside Flow \"" + flow + "\"";
            std::string rest = path.substr(flow.size() + 1);
            std::size_t lastDot = rest.rfind('.');
            std::string leaf = lastDot == std::string::npos ? rest : rest.substr(lastDot + 1);

            ::ModuleInfoList* list = Sdk("Read Flow module list", [&] { return proc->GetAllModuleList(); });
            if (!list) return "SDK returned no module list for Flow \"" + flow + "\"";
            std::string groups;
            for (unsigned i = 0; i < list->nNum && i < std::size(list->astModuleInfo); ++i)
            {
                std::string type = Str(list->astModuleInfo[i].strModuleName);
                std::string display = Str(list->astModuleInfo[i].strDisplayName);
                if (display == leaf)
                    return IsGroupType(type) ? std::string() : "Module \"" + leaf + "\" in Flow \"" + flow + "\" is not a Group (type " + type + ")";
                if (IsGroupType(type)) groups += (groups.empty() ? "" : ", ") + display;
            }
            return "Group \"" + leaf + "\" not found in Flow \"" + flow + "\"" + (groups.empty() ? std::string(" — Flow has no Group") : " — Groups in this Flow: " + groups);
        }

        /// Group đã đối chiếu bằng <see cref="FindGroup"/> rồi mới ép void* của operator[] (trả void* cho mọi module).
        /// SDK: IVmSolution::operator["Flow.Group"] (mẫu GetResultControlDlg.cpp ép kiểu y như vậy).
        GroupTool* GetGroup(Procedure* proc, const std::string& flow, const std::string& path)
        {
            std::string why = FindGroup(proc, flow, path);
            if (!why.empty()) throw VisionDeviceError(why);
            void* p = nullptr;
            try { p = (*sol)[path.c_str()]; }
            catch (const vm::CVmException& e) { throw VisionDeviceError(Describe("Find Group \"" + path + "\"", e), e.GetErrorCode()); }
            catch (...) { throw VisionDeviceError("Find Group \"" + path + "\": VisionMaster SDK threw unknown error", -2); }
            if (!p) throw VisionDeviceError("Group \"" + path + "\" not found in solution");
            return static_cast<GroupTool*>(p);
        }

        /// SDK: IVmSolution::SaveSolution (IVmSolution.h) — lưu vào chính file .solw đang nạp.
        void Save() { Sdk("Save solution", [&] { sol->SaveSolution(); }); }

        void DisconnectLocked()
        {
            if (sol && !g_sdkDisposed.load())
            {
                try { sol->UnRegisterCallBack(); } catch (...) {}
                if (loaded.load())
                {
                    // SDK: IMVS_PF_CloseSolution(handle) (iMVS-6000PlatformSDKC.h) với handle từ
                    // IVmSolution::GetSolutionHandle — tương đương CloseSolution() bản .NET. Chưa chạy được ở máy dev.
                    try { if (void* h = sol->GetSolutionHandle()) ::IMVS_PF_CloseSolution(h); } catch (...) {}
                }
                // SDK: DestroySolutionInstance (IVmSolution.h). Đo ở máy dev: tạo → huỷ → tạo lại 3 vòng đều được.
                try { ::DestroySolutionInstance(sol); } catch (...) {}
            }
            sol = nullptr;
            renderParams.reset();
            loaded = false;
            connected = false;
            std::lock_guard<std::mutex> l(cacheMutex);
            flowNames.clear();
        }
    };

    VisionMasterDevice::VisionMasterDevice(std::function<std::shared_ptr<SystemData>()> sys) : m(std::make_unique<Impl>())
    {
        m->sys = std::move(sys);
    }

    /// Huỷ instance solution nếu còn (không gọi DisposeResource — hãng dặn tránh gọi nó trong destructor).
    VisionMasterDevice::~VisionMasterDevice()
    {
        try { Disconnect(); } catch (...) {}
    }

    std::shared_ptr<IVisionDevice> VisionMasterDevice::Create(std::function<std::shared_ptr<SystemData>()> sys)
    {
        return std::make_shared<VisionMasterDevice>(std::move(sys));
    }

    bool VisionMasterDevice::IsConnected() const { return m->connected.load(); }
    bool VisionMasterDevice::IsSolutionLoaded() const { return m->loaded.load(); }

    bool VisionMasterDevice::Connect()
    {
        std::lock_guard<std::recursive_timed_mutex> lock(m->sdk);
        if (m->sol) { m->connected = true; return true; }

        RequireSdkDll();
        if (g_sdkDisposed.load())
            throw VisionDeviceError("VisionMaster SDK already released (DisposeSdk) — restart the program to reconnect", CodeInvalidHandle);

        // SDK: GetSolutionExistedInstance / CreateSolutionInstance (IVmSolution.h; mẫu GetResultControlDlg.cpp).
        // Instance còn sống thì dùng lại: tạo lần hai làm RegisterCallBack ném lỗi và SDK sập ở lời gọi kế (đo ở máy dev).
        Solution* sol = Sdk("Get solution instance", [] { return ::GetSolutionExistedInstance(); });
        if (!sol) sol = Sdk("Create solution instance (CreateSolutionInstance)", [] { return ::CreateSolutionInstance(); });
        if (!sol) throw VisionDeviceError("CreateSolutionInstance returned null");
        m->sol = sol;

        // App dùng chuỗi UTF-8 khắp nơi (đường dẫn có dấu, tên Flow) — header: "Should be called after
        // CreateSolutionInstance". Không đặt được thì vẫn chạy với tên ASCII.
        try { ::SetGlobalEncoding(IMVS_ENCODING_UTF8); } catch (...) {}

        try
        {
            Sdk("Register callback", [&] { sol->RegisterCallBack(OnSdkCallback, m->signal.get()); });
            // Tắt callback kết quả của mọi module để đỡ tốn tài nguyên — chỉ bật cho Flow cần ảnh.
            Sdk("Disable module callback", [&] { sol->DisableModulesCallback(); });
        }
        catch (...)
        {
            m->DisconnectLocked();
            throw;
        }
        m->connected = true;
        return true;
    }

    void VisionMasterDevice::Disconnect()
    {
        std::lock_guard<std::recursive_timed_mutex> lock(m->sdk);
        m->DisconnectLocked();
    }

    void VisionMasterDevice::DisposeSdk()
    {
        if (g_sdkDisposed.exchange(true)) return;
        if (GetModuleHandleW(L"iMVS-6000PlatformSDK.dll") == nullptr) return;   // SDK chưa từng được nạp
        try { ::DisposeResource(); } catch (...) {}
    }

    bool VisionMasterDevice::LoadSolution(const std::string& solutionPath)
    {
        std::lock_guard<std::recursive_timed_mutex> lock(m->sdk);
        if (!m->sol) throw VisionDeviceError("VisionMaster SDK not connected (Connect not called)");

        // SDK: LoadSolution(path, password) (IVmSolution.h; mẫu GetResultControlDlg.cpp, VmSdk.cpp). Solution là
        // singleton toàn tiến trình nên chỉ một solution được nạp tại một thời điểm.
        Solution* sol = Sdk("Load solution", [&] { return ::LoadSolution(solutionPath.c_str(), ""); });
        if (!sol) throw VisionDeviceError("Load solution: SDK returned null solution");
        m->sol = sol;
        m->renderParams.reset();     // solution mới thì module cũ không còn hiệu lực
        m->runsWithoutCallback = 0;
        { std::lock_guard<std::mutex> l(m->signal->mutex); m->signal->everSeen = false; }

        // Sau khi nạp xong, tắt callback kết quả của mọi module (mẫu GetResultControl làm y hệt ngay sau LoadSolution).
        try { sol->DisableModulesCallback(); } catch (...) {}
        m->RefreshFlowNames();
        m->loaded = true;
        return true;
    }

    std::string VisionMasterDevice::CheckModulePath(const std::string& path)
    {
        std::lock_guard<std::recursive_timed_mutex> lock(m->sdk);
        if (!m->sol || !m->loaded) return "Solution not loaded";

        std::string p = base::Trim(path);
        std::string flow = base::Trim(p.substr(0, p.find('.')));
        bool hasId = false;
        unsigned id = 0;
        Procedure* proc = m->GetProcedure(flow, hasId, id);   // không có Flow thì ném, VisionConnect trả lý do
        return m->FindGroup(proc, flow, p);
    }

    VmCallResult VisionMasterDevice::CallFlow(const VmCallRequest& request, std::stop_token ct)
    {
        if (request.Flow.empty()) return VmCallResult::Fail("Missing Flow name");
        VisionOutputNames names = m->Names();

        std::lock_guard<std::recursive_timed_mutex> lock(m->sdk);
        if (!m->sol || !m->loaded) return VmCallResult::Fail("Solution not loaded");

        bool hasId = false;
        unsigned id = 0;
        Procedure* proc = m->GetProcedure(request.Flow, hasId, id);
        // Group tra TRƯỚC khi chạy: đường dẫn sai thì không kích camera. LoadSolution đã tắt callback kết quả của mọi
        // module — bật lại cho riêng Group này, tắt thì GetResult() của nó không có số của lần chạy vừa rồi
        // (CVmToolBase::EnableResultCallback, VmToolBase.h).
        GroupTool* group = request.Module.empty() ? nullptr : m->GetGroup(proc, request.Flow, request.Module);
        if (group) Sdk("Enable Group result callback", [&] { group->EnableResultCallback(); });
        bool wantImage = request.Camera != EVmCamera::None && !names.OutImage.empty();
        ProcResults* res = m->RunFlow(proc, hasId, id, wantImage, ct);

        // SDK: IMVSGroupTool::GetResult() — NULL khi Group không chạy hoặc chạy lỗi ở lần này (IVmGroup.h).
        GroupResults* groupRes = group ? Sdk("Get Group result", [&] { return group->GetResult(); }) : nullptr;
        if (group && !groupRes) throw VisionDeviceError("Group \"" + request.Module + "\" did not run or failed in this run — no result");

        VmCallResult result;
        for (auto& name : request.Outputs)
        {
            float v = 0.0f;
            bool ok = group ? TryReadNumber(groupRes, name, v) : TryReadNumber(res, name, v);
            if (!ok)
                throw VisionDeviceError("\"" + (group ? request.Module : request.Flow) + "\" missing output \"" + name +
                                        "\" — check its Output Params (int or float)");
            result.Values[name] = v;
        }

        std::shared_ptr<const ImageData> image = wantImage ? TryReadImage(res, names.OutImage) : nullptr;
        if (request.Camera != EVmCamera::None) m->SetImage(image, request.Nozzle);

        result.Ok = true;
        return result;
    }

    std::string VisionMasterDevice::SaveSolution()
    {
        std::lock_guard<std::recursive_timed_mutex> lock(m->sdk);
        if (!m->sol || !m->loaded) return "Solution not loaded";
        try { m->Save(); return {}; }
        catch (const std::exception& ex) { std::string w = ex.what(); return w.empty() ? "Save solution failed" : w; }
    }

    std::shared_ptr<const ImageData> VisionMasterDevice::LastImage() const
    {
        std::lock_guard<std::mutex> l(m->cacheMutex);
        return m->image;
    }

    int VisionMasterDevice::LastImageNozzle() const
    {
        std::lock_guard<std::mutex> l(m->cacheMutex);
        return m->imageNozzle;
    }

    void* VisionMasterDevice::RenderSource(const std::string& path)
    {
        std::unique_lock<std::recursive_timed_mutex> lock(m->sdk, UiLockWait);
        if (!lock.owns_lock() || !m->sol || !m->loaded || path.empty()) return nullptr;

        try
        {
            std::size_t dot = path.find('.');
            std::string flow = path.substr(0, dot);
            bool hasId = false;
            unsigned id = 0;
            Procedure* proc = m->GetProcedure(flow, hasId, id);

            // Cả Flow — SDK: IVmProcedure::GetControlInfo() (IVmProcedure.h). Chưa chạy được ở máy dev.
            if (dot == std::string::npos) return Sdk("Get Flow ControlInfo", [&] { return proc->GetControlInfo(); });

            // Một module: tra ID theo tên hiển thị trong IVmProcedure::GetAllModuleList() (trả cả module nằm
            // trong Group), rồi dựng CModuParamsBase(moduleID) và gọi GetControlInfo() của nó
            // (VmModuleParamBase.h) — đúng hàm mà mẫu VC gọi qua lớp tham số riêng của từng module
            // (pCirFindParam->GetControlInfo(); các lớp đó thừa kế CModuParamsBase và không định nghĩa lại hàm này).
            // Đã chạy ở máy dev khi KHÔNG có solution: trả con trỏ khác null trỏ vào vùng tĩnh của DLL.
            std::string rest = path.substr(dot + 1);
            std::size_t lastDot = rest.rfind('.');
            std::string leaf = lastDot == std::string::npos ? rest : rest.substr(lastDot + 1);

            ::ModuleInfoList* list = Sdk("Read Flow module list", [&] { return proc->GetAllModuleList(); });
            if (!list) return nullptr;
            for (unsigned i = 0; i < list->nNum && i < std::size(list->astModuleInfo); ++i)
            {
                std::string name = Str(list->astModuleInfo[i].strDisplayName);
                if (name.empty() || (name != rest && name != leaf)) continue;
                unsigned moduleId = list->astModuleInfo[i].nModuleID;
                m->renderParams = Sdk("Build module params", [&] { return std::make_unique<vm::CModuParamsBase>(moduleId); });
                return Sdk("Get module ControlInfo", [&] { return m->renderParams->GetControlInfo(); });
            }
            return nullptr;
        }
        catch (...) { return nullptr; }
    }

    std::vector<std::string> VisionMasterDevice::FlowNames()
    {
        {
            std::unique_lock<std::recursive_timed_mutex> lock(m->sdk, UiLockWait);
            if (lock.owns_lock())
            {
                if (!m->sol || !m->loaded) return {};
                m->RefreshFlowNames();
            }
        }
        std::lock_guard<std::mutex> l(m->cacheMutex);
        return m->flowNames;
    }

    std::string VisionMasterDevice::SdkVersion()
    {
        if (!EnsureSdkDll()) return {};
        try
        {
            const char* v = ::IMVS_PF_GetBuildVersion();   // iMVS-6000PlatformSDKC.h
            return v ? std::string(v) : std::string();
        }
        catch (...) { return {}; }
    }
}
