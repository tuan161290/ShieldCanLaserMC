#include "Controller/McControl.h"

#include <atomic>
#include <format>
#include <mutex>
#include <thread>
#include <windows.h>

#include "Base/Async.h"
#include "Base/Text.h"
#include "Communication/EtherCat/EtherCatIoDevice.h"
#include "Communication/Sim/FakeIo.h"
#include "Controller/VmCycle.h"
#include "Data/McDataManager.h"
#include "Infor/AlarmManager.h"
#include "Infor/IoMap.h"
#include "Utility/Log/LogWorker.h"

namespace scs
{
    namespace
    {
        const char* const HeldMessage = "Paused — press START to resume, switch to MANUAL to abort cycle";

        void Log(const std::string& msg, LogLevel level, const char* source = "McControl")
        {
            LogWorker::Instance().RecordLog(msg, level, source);
        }

        std::string ReplaceNewLines(std::string text)
        {
            for (size_t p = 0; (p = text.find("\r\n", p)) != std::string::npos; p += 3) text.replace(p, 2, " | ");
            return text;
        }

        struct State
        {
            McStatus status;
            MotionConnect motion;

            std::mutex devMutex;   // các con trỏ dưới đây: Init() ghi, luồng khác đọc qua accessor
            std::shared_ptr<IIoDevice> io;
            std::shared_ptr<VisionConnect> vision;
            std::shared_ptr<SafetyMonitor> safety;
            std::shared_ptr<McFunction> func;
            std::shared_ptr<Routine_Origin> origin;
            std::shared_ptr<Routine_Auto> autoR;
            std::shared_ptr<Routine_Calib> calib;

            base::Event<std::string> autoModeRefused;
            std::atomic<bool> initialized{ false };
            std::atomic<bool> running{ false };
            std::atomic<long long> lastTickMs{ 0 }, loopStartMs{ 0 };
            base::Stopwatch clock;

            // Yêu cầu từ UI / nút cứng — cờ một lần, vòng quét đọc rồi xoá.
            std::atomic<int> reqStart{ 0 }, reqStop{ 0 }, reqPause{ 0 }, reqReset{ 0 }, reqOrigin{ 0 }, reqInit{ 0 }, reqMute{ 0 }, reqArm{ 0 };

            /// Chế độ Auto/Manual đặt bằng phần mềm (nút trên trang Main). Trạng thái BỀN, vòng quét chỉ
            /// đọc ra so với Status.Mode.
            std::atomic<int> requestedMode{ (int)EMcMode::Manual };
            std::atomic<int> visionLoadCount{ 0 };

            std::mutex routineLock;
            std::shared_ptr<std::atomic<bool>> routineDone;   // null = chưa từng chạy routine nào

            /// Token riêng của chu trình Auto đang chạy — nối vào token Safety lúc khởi động, EndCycle
            /// huỷ được cả khi Safety đã đổi token sau RESET.
            std::shared_ptr<base::LinkedStopSource> cycleCts;
            /// Routine đang chạy là chu trình Auto (không phải Về gốc/Init); đã xin kết thúc nó chưa.
            bool activeIsCycle{ false };
            std::atomic<bool> cycleEnding{ false };

            // Luồng khai báo SAU CÙNG: huỷ trước mọi thứ chúng dùng.
            std::jthread routine, loop, watchdog;
        };

        State& St() { static State s; return s; }
    }

    struct McControl::Impl
    {
        // ───────────────────────── routine nền ─────────────────────────

        /// Gọi trong routineLock, lúc routine cũ đã thoát.
        template <class F>
        static void StartRoutine(State& s, F body)
        {
            auto done = std::make_shared<std::atomic<bool>>(false);
            if (s.routine.joinable()) s.routine.join();
            s.routineDone = done;
            s.routine = std::jthread([done, body = std::move(body)]
            {
                try { body(); } catch (...) { }
                done->store(true);
            });
        }

        static bool CycleBusy(State& s)
        {
            std::lock_guard<std::mutex> lock(s.routineLock);
            return s.activeIsCycle && s.routineDone && !s.routineDone->load();
        }

        /// Gọi trong routineLock, lúc chu trình cũ đã thoát.
        static std::stop_token NewCycleToken(State& s)
        {
            s.cycleCts = std::make_shared<base::LinkedStopSource>(s.safety->AbortToken());
            s.activeIsCycle = true;
            s.cycleEnding = false;
            return s.cycleCts->Token();
        }

        // ───────────────────────── vision ─────────────────────────

        /// Nạp solution vision ngay lúc khởi tạo, chạy nền để không giữ màn hình khởi động. Quên bước này thì
        /// mọi lần chụp đều trả "không tìm thấy đối tượng" mà trên màn hình chỉ thấy alarm vision.
        static void LoadVisionSolution(State& s, const SystemData& sys)
        {
            std::string path = sys.VisionSolutionPath;
            if (base::IsBlank(path))
            {
                Log("Vision solution path not set (Setting page)", LogLevel::Warning, "Vision");
                return;
            }

            auto vision = Vision();
            State* sp = &s;
            base::RunDetached([vision, path, sp]
            {
                try
                {
                    bool ok = vision->LoadSolution(path);
                    if (ok)
                    {
                        sp->visionLoadCount.fetch_add(1);
                        Log("Vision solution loaded: " + path, LogLevel::Info, "Vision");
                        // Raise ở nhánh lỗi không có nơi nào gỡ — phải gỡ ở đây, không thì một lần nạp lỗi
                        // là alarm này sống mãi dù nạp lại thành công.
                        AlarmManager::ClearCondition(AlarmCode::VisionLoadFailed);
                    }
                    else
                    {
                        AlarmManager::Raise(AlarmCode::VisionLoadFailed, "Vision", path);
                    }
                }
                catch (const std::exception& ex)
                {
                    AlarmManager::Raise(AlarmCode::VisionLoadFailed, "Vision", ex.what());
                }
            });
        }

        // ───────────────────────── vòng quét ─────────────────────────

        static void StartLoop(State& s, int tickMs)
        {
            if (s.running) return;
            s.running = true;
            s.lastTickMs = s.clock.ElapsedMilliseconds();
            s.loopStartMs = s.lastTickMs.load();

            s.loop = std::jthread([&s, tickMs](std::stop_token st)
            {
                ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
                Loop(s, tickMs, st);
            });
            s.watchdog = std::jthread([&s](std::stop_token st)
            {
                if (!base::DelayNoThrow(1000, st)) return;
                while (!st.stop_requested())
                {
                    CheckWatchdog(s);
                    if (!base::DelayNoThrow(200, st)) return;
                }
            });
        }

        static void Loop(State& s, int tickMs, std::stop_token st)
        {
            base::Stopwatch tickSw;
            while (s.running && !st.stop_requested())
            {
                tickSw.Restart();
                try
                {
                    s.motion.Refresh();
                    s.io->Refresh();
                    s.safety->Evaluate();

                    UpdateMode(s);
                    HandleButtons(s);
                    UpdateState(s);
                    EMcState state = s.status.State();
                    // Tạm dừng mà vào LỖI/EMG thì bỏ chu trình — sau RESET không còn gì để START chạy tiếp.
                    if (!s.func->RunBit() && (state == EMcState::Alarm || state == EMcState::Emg))
                        AbandonHeld(s, "error while paused");
                    // Chu trình đang chạy mà vào LỖI/EMG thì cho nó thoát ở điểm dừng kế tiếp: RESET xong máy về
                    // DỪNG, chu trình không được chạy tiếp ngầm phía sau.
                    state = s.status.State();
                    if ((state == EMcState::Alarm || state == EMcState::Emg) && CycleBusy(s) && !s.cycleEnding)
                        EndCycle(s, "error while running", false);
                    UpdateOutputs(s);
                }
                catch (const std::exception& ex)
                {
                    // Sự kiện, không Raise trơn: không nơi nào gỡ nguyên nhân, một lỗi là RESET không bao giờ xoá được.
                    AlarmManager::RaiseEvent(AlarmCode::RoutineCrashed, "McControl", ex.what());
                }
                catch (...)
                {
                    AlarmManager::RaiseEvent(AlarmCode::RoutineCrashed, "McControl", "unknown error");
                }
                s.lastTickMs = s.clock.ElapsedMilliseconds();

                int rest = tickMs - (int)tickSw.ElapsedMilliseconds();
                base::Delay(rest > 0 ? rest : 1);
            }
        }

        /// Watchdog chạy trên luồng RIÊNG — vòng quét treo thì nó không tự phát hiện được. Có ân hạn khởi động
        /// (WatchdogStartupGraceMs): tick đầu có thể trễ vì nạp solution vision chiếm CPU.
        static void CheckWatchdog(State& s)
        {
            if (!s.running) return;

            auto sys = McDataManager::SysData();
            long long now = s.clock.ElapsedMilliseconds();
            long long silent = now - s.lastTickMs.load();
            int limit = sys->WatchdogTimeoutMs;

            if (silent > limit)
            {
                bool inStartupGrace = now - s.loopStartMs.load() < sys->WatchdogStartupGraceMs;
                if (!inStartupGrace)
                    AlarmManager::Raise(AlarmCode::ControlWatchdog, "System", std::format("silent {} ms > {} ms", silent, limit));
            }
            else
            {
                AlarmManager::ClearCondition(AlarmCode::ControlWatchdog);
            }
        }

        // ───────────────────────── chế độ / nút bấm ─────────────────────────

        static void UpdateMode(State& s)
        {
            McStatus& status = s.status;
            auto next = (EMcMode)s.requestedMode.load();
            if (next == status.Mode()) return;

            // Chuyển về Manual lúc đang chạy thì chạy nốt tới checkpoint rồi dừng. Đang tạm dừng thì bỏ chu
            // trình: ở Manual còn jog được, START sau đó mà phát lại lệnh dở từ chỗ đã jog tới là va.
            if (status.Mode() == EMcMode::Auto && (status.State() == EMcState::Running || status.State() == EMcState::WaitOperator))
                RequestPause();
            if (next == EMcMode::Manual && status.State() == EMcState::Held)
                AbandonHeld(s, "switched to MANUAL while paused");

            // Armed chỉ có nghĩa dưới Mode Auto — rời Auto thì huỷ luôn.
            if (next == EMcMode::Manual && status.State() == EMcState::Armed)
                status.SetState(EMcState::Idle);

            // Chọn AUTO là vào luôn trạng thái sẵn sàng. Chưa đủ điều kiện thì ở lại MANUAL và báo ngay.
            EMcState st = status.State();
            bool cycleAlive = st == EMcState::Running || st == EMcState::Pausing || st == EMcState::WaitOperator;
            if (next == EMcMode::Auto && !cycleAlive)
            {
                std::string why = ArmBlockReason(s);
                if (!why.empty())
                {
                    s.requestedMode = (int)EMcMode::Manual;
                    std::string msg = "Cannot enter AUTO — " + why;
                    status.SetMessage(msg);
                    Log(msg, LogLevel::Warning);
                    s.autoModeRefused.Raise(msg);
                    return;
                }
                status.SetMode(next);
                Log(std::string("Mode changed: ") + ToString(next), LogLevel::Info);
                Arm(s, "switch to AUTO");
                return;
            }

            status.SetMode(next);
            Log(std::string("Mode changed: ") + ToString(next), LogLevel::Info);
        }

        static void HandleButtons(State& s)
        {
            // Nút cứng bắt SƯỜN LÊN, không bắt mức — nút kẹt hoặc dính tiếp điểm mà bắt theo mức sẽ khiến máy
            // tự khởi động lại liên tục.
            if (s.io->RisingEdge(IoMap::BtnStart)) RequestStart();
            if (s.io->RisingEdge(IoMap::BtnStop)) RequestStop();
            if (s.io->RisingEdge(IoMap::BtnReset)) RequestReset();

            if (s.reqReset.exchange(0) == 1) DoReset(s);
            if (s.reqMute.exchange(0) == 1) s.status.SetBuzzerMuted(true);
            if (s.reqStop.exchange(0) == 1) DoStop(s);
            if (s.reqPause.exchange(0) == 1) DoPause(s);
            if (s.reqOrigin.exchange(0) == 1) DoOrigin(s);
            if (s.reqInit.exchange(0) == 1) DoInit(s);
            if (s.reqArm.exchange(0) == 1) DoArm(s);
            if (s.reqStart.exchange(0) == 1) DoStart(s);
        }

        /// Lỗi nào cũng phải bấm RESET xác nhận rồi mới cho chạy tiếp. Chỉ xét alarm đã hết nguyên nhân —
        /// alarm còn nguyên nhân thì Error đã tự chặn qua trạng thái ALARM.
        static std::string PendingResetText()
        {
            auto a = AlarmManager::PendingReset();
            if (!a) return {};
            return "Unacknowledged error: " + std::to_string(a->Code) + " " + a->Message() + " — press RESET first";
        }

        static bool AlarmsAcknowledged(State& s)
        {
            std::string text = PendingResetText();
            if (text.empty()) return true;
            s.status.SetMessage(text);
            return false;
        }

        /// Nút START mềm — vào lại sẵn sàng khi đã ở AUTO (vd. sau STOP). Chọn AUTO thì tự vào.
        static void DoArm(State& s)
        {
            if (s.status.State() == EMcState::Armed) return;
            if (s.status.Mode() != EMcMode::Auto)
            {
                s.status.SetMessage("Select AUTO mode first");
                return;
            }
            std::string why = ArmBlockReason(s);
            if (!why.empty()) { s.status.SetMessage(why); return; }
            Arm(s, "on-screen START button");
        }

        /// Lý do chưa vào được trạng thái sẵn sàng; rỗng = vào được. Chọn AUTO và nút START mềm cùng xét
        /// ở đây nên hai đường không lệch nhau.
        static std::string ArmBlockReason(State& s)
        {
            EMcState st = s.status.State();
            if (st == EMcState::Held) return HeldMessage;
            if (RoutineBusy())
                return st == EMcState::Idle || st == EMcState::Paused || st == EMcState::NotHomed
                    ? "Cycle in progress — press STOP to cancel"
                    : "Cycle in progress";
            switch (st)
            {
                case EMcState::Idle:
                case EMcState::Paused:
                    break;
                case EMcState::NotHomed:
                case EMcState::Init:
                    return "Not homed — press Origin first";
                case EMcState::Alarm:
                case EMcState::Emg:
                    return "Error active — clear the cause, then press RESET";
                default:
                    return "Machine state: " + s.status.StateText();
            }
            std::string pending = PendingResetText();
            if (!pending.empty()) return pending;
            if (McDataManager::RunData()->UnsafePause)
                return "Last stop was mid-motion — press Init (next to Origin) to move to standby";
            return {};
        }

        static void Arm(State& s, const std::string& how)
        {
            s.status.SetState(EMcState::Armed);
            s.status.SetMessage("Ready — press START on the control panel to run");
            Log("Armed (" + how + ")", LogLevel::Info);
        }

        /// Nút START vật lý. Tạm dừng → bật lại bit RUN, chu trình chạy tiếp đúng chỗ dở. Armed → khởi động chu
        /// trình. KHÔNG có nhánh NotHomed→Về gốc: về gốc là chuyển động thật, phải bấm nút "Về gốc" riêng.
        static void DoStart(State& s)
        {
            McStatus& status = s.status;
            if (status.State() == EMcState::Held)
            {
                if (!s.safety->SafeToMove()) { status.SetMessage("Cannot resume: " + s.safety->BlockReason()); return; }
                s.autoR->HoldClock(false);
                status.SetState(EMcState::Running);
                status.SetMessage("Auto running");
                s.func->SetRunBit(true);
                Log("START — RUN bit ON, resuming", LogLevel::Info);
                return;
            }

            // Chu trình đang chờ thay tray: START = "đã thay tray, chạy tiếp từ hàng 1".
            if (CycleBusy(s) && s.autoR->WaitingTray)
            {
                s.autoR->TrayStartRequest = true;
                return;
            }

            if (RoutineBusy()) { status.SetMessage("Cycle in progress"); return; }

            if (status.State() != EMcState::Armed)
            {
                status.SetMessage(status.Mode() == EMcMode::Auto
                    ? "Cannot run in state " + status.StateText()
                    : std::string("Select AUTO mode before pressing START"));
                return;
            }

            if (!AlarmsAcknowledged(s)) return;
            if (!s.safety->SafeToMove()) { status.SetMessage("Cannot run: " + s.safety->BlockReason()); return; }

            StartAuto(s);
        }

        static void StartAuto(State& s)
        {
            s.safety->RenewToken();
            s.func->SetRunBit(true);
            s.autoR->PauseRequested = false;
            McDataManager::RunData()->UnsafePause = false;

            s.status.SetState(EMcState::Running);
            s.status.SetMessage("Auto running");

            std::lock_guard<std::mutex> lock(s.routineLock);
            auto ct = NewCycleToken(s);
            auto autoR = s.autoR; auto func = s.func; McStatus* status = &s.status;
            StartRoutine(s, [autoR, func, status, ct]
            {
                bool ok;
                try { ok = autoR->Run(ct); }
                catch (const base::OperationCanceled&) { ok = false; }
                catch (const std::exception& ex)
                {
                    AlarmManager::RaiseEvent(AlarmCode::RoutineCrashed, "Auto", ex.what());
                    ok = false;
                }
                func->SetRunBit(true);   // chu trình đã thoát — bit tắt thì Về gốc sau đó đứng chờ mãi

                // Routine tự đặt Paused khi dừng có kiểm soát. Mọi đường ra khác đều là dừng bất thường
                // và không được để máy trông như đang sẵn sàng.
                EMcState st = status->State();
                if (st == EMcState::Running || st == EMcState::Pausing || st == EMcState::WaitOperator || st == EMcState::Held)
                    status->SetState(ok ? EMcState::Idle : EMcState::Paused);

                McDataManager::SaveRunningData();
            });
        }

        static void DoStop(State& s)
        {
            McStatus& status = s.status;
            switch (status.State())
            {
                case EMcState::Homing:
                    // Huỷ về gốc: OrgOk về false, không bao giờ để trạng thái dở dang là "đã về gốc".
                    s.safety->RequestAbort("operator pressed STOP during origin");
                    status.SetMessage("Origin sequence cancelled");
                    return;

                case EMcState::WaitOperator:
                    DoPause(s);
                    return;

                case EMcState::Running:
                    // Tắt bit RUN: mọi trục dừng ngay, chu trình đứng nguyên chỗ, START chạy tiếp. GIỮ servo.
                    Hold(s, HeldMessage, "STOP");
                    return;

                case EMcState::Held:
                    status.SetMessage(HeldMessage);
                    return;

                case EMcState::Pausing:
                    // Đang chạy nốt tới checkpoint (sau khi chuyển MANUAL): STOP thì dừng ngay tại chỗ, GIỮ servo.
                    s.safety->RequestAbort("operator pressed STOP twice");
                    s.motion.StopAll();
                    McDataManager::RunData()->UnsafePause = true;
                    McDataManager::SaveRunningData();
                    status.SetState(EMcState::Paused);
                    status.SetMessage("Stopped immediately — press Init before resuming");
                    Log("STOP twice — immediate stop, UnsafePause set", LogLevel::Warning);
                    return;

                case EMcState::Armed:
                    status.SetState(EMcState::Idle);
                    status.SetMessage("Exited ready state");
                    return;

                default:
                    s.motion.StopAll();
                    if (CycleBusy(s)) EndCycle(s, "operator pressed STOP", true);
                    return;
            }
        }

        /// Kết thúc chu trình còn sống: cờ dừng cho mọi checkpoint. cancelToken huỷ luôn
        /// token riêng của chu trình — dừng cả lệnh trục/Flow đang chạy.
        static void EndCycle(State& s, const std::string& why, bool cancelToken)
        {
            s.cycleEnding = true;
            s.autoR->PauseRequested = true;
            if (cancelToken)
            {
                std::shared_ptr<base::LinkedStopSource> cts;
                { std::lock_guard<std::mutex> lock(s.routineLock); cts = s.cycleCts; }
                if (cts) cts->Cancel();
            }
            std::string msg = "Cycle cancelled (" + why + ")";
            s.status.SetMessage(msg);
            Log(msg, LogLevel::Warning);
        }

        /// Tạm dừng chu trình đang chạy: tắt bit RUN, trục dừng tại chỗ, START chạy tiếp.
        static void Hold(State& s, const std::string& message, const std::string& why)
        {
            s.func->SetRunBit(false);
            s.autoR->HoldClock(true);
            s.status.SetState(EMcState::Held);
            s.status.SetMessage(message);
            Log(why + " — RUN bit OFF, axes stopped in place", LogLevel::Info);
        }

        /// Rèm sáng vừa bị cắt — SafetyMonitor gọi trên vòng quét, TRƯỚC khi SafeToMove tắt: chu trình
        /// Auto tạm dừng y như STOP; về gốc/Init thì huỷ.
        static void HoldForCurtain(State& s)
        {
            switch (s.status.State())
            {
                case EMcState::Running:
                    Hold(s, "Photo safety triggered — stopped. Clear the area, then press START to resume", "Photo safety");
                    return;
                case EMcState::Pausing:
                case EMcState::Homing:
                    DoStop(s);
                    return;
                default:
                    return;
            }
        }

        /// Dừng ở checkpoint (RequestPause): routine chạy nốt bước đang làm rồi thoát.
        static void DoPause(State& s)
        {
            switch (s.status.State())
            {
                case EMcState::WaitOperator:
                    s.autoR->PauseRequested = true;
                    s.status.SetMessage("Operator wait cancelled");
                    return;

                case EMcState::Running:
                    s.autoR->PauseRequested = true;
                    s.status.SetState(EMcState::Pausing);
                    s.status.SetMessage("Finishing to safe stop point…");
                    Log("Stopping at checkpoint", LogLevel::Info);
                    return;
                default:
                    return;
            }
        }

        /// Bỏ chu trình đang tạm dừng (chuyển MANUAL, có lỗi). Trục đứng giữa hành trình nên đánh dấu
        /// UnsafePause — lần chạy sau đòi Init trước.
        static void AbandonHeld(State& s, const std::string& why)
        {
            s.safety->RequestAbort(why);
            s.func->SetRunBit(true);
            McDataManager::RunData()->UnsafePause = true;
            McDataManager::SaveRunningData();
            if (s.status.State() == EMcState::Held)
            {
                s.status.SetState(EMcState::Paused);
                s.status.SetMessage("Paused cycle aborted (" + why + ") — press Init to move to standby before resuming");
            }
            Log("Paused cycle aborted — " + why, LogLevel::Warning);
        }

        static void DoReset(State& s)
        {
            s.status.SetBuzzerMuted(true);

            // Bản C# phát ResetAlarmAsync rồi không chờ; ở đây lệnh reset là hàm chặn nên đẩy sang luồng nền
            // để vòng quét không đứng chờ driver.
            for (const auto& axis : s.motion.AxisList())
                if (axis->Alarm())
                    base::RunDetached([axis] { axis->ResetAlarm(); });

            auto stuck = AlarmManager::Reset();
            s.safety->RenewToken();

            if (!stuck.empty())
            {
                // Bấm RESET mà không xoá được gì thì phải nói rõ vì sao — đây là lúc operator hay bấm loạn nhất.
                s.status.SetMessage("RESET: cannot clear — " + stuck[0].Message() +
                                    (stuck.size() > 1 ? std::format(" (and {} more)", stuck.size() - 1) : std::string()));
                return;
            }

            s.status.SetMessage("RESET done");
            if (s.status.State() == EMcState::Alarm || s.status.State() == EMcState::Emg)
                s.status.SetState(s.status.OrgOk() ? EMcState::Idle : EMcState::NotHomed);
        }

        static void DoOrigin(State& s)
        {
            McStatus* status = &s.status;
            if (RoutineBusy()) { status->SetMessage("Cycle in progress"); return; }
            if (!s.safety->SafeToMove()) { status->SetMessage("Cannot home: " + s.safety->BlockReason()); return; }

            s.safety->RenewToken();
            status->SetState(EMcState::Homing);
            status->SetMessage("Origin sequence started");

            auto ct = s.safety->AbortToken();
            auto origin = s.origin;
            std::lock_guard<std::mutex> lock(s.routineLock);
            s.activeIsCycle = false;
            StartRoutine(s, [origin, status, ct]
            {
                bool ok = origin->Run(ct);
                if (ok) McDataManager::RunData()->UnsafePause = false;   // mọi trục đã ở vị trí chờ, như Init
                status->SetState(ok ? EMcState::Idle : EMcState::NotHomed);
                // Routine chỉ đặt Message khi thất bại — thành công cũng phải nói.
                if (ok) status->SetMessage("Origin complete — machine ready");
            });
        }

        /// Init: nâng Z rồi đưa mọi trục về vị trí chờ, không về gốc lại — dùng sau khi tạm dừng/thao tác
        /// tay, trước khi chạy tiếp. Chạy dưới trạng thái Homing để STOP, đèn và chốt an toàn xử lý y như về gốc.
        static void DoInit(State& s)
        {
            McStatus* status = &s.status;
            if (status->State() == EMcState::Held)
            {
                status->SetMessage("Paused — switch to MANUAL to abort cycle, then press Init");
                return;
            }
            if (RoutineBusy()) { status->SetMessage("Cycle in progress"); return; }
            if (!status->OrgOk()) { status->SetMessage("Cannot Init: not homed — press Origin"); return; }
            if (!s.safety->SafeToMove()) { status->SetMessage("Cannot Init: " + s.safety->BlockReason()); return; }

            s.safety->RenewToken();
            EMcState before = status->State();
            status->SetState(EMcState::Homing);
            status->SetMessage("Init — moving to standby");
            Log("Init — moving to standby", LogLevel::Info);

            auto ct = s.safety->AbortToken();
            auto origin = s.origin;
            std::lock_guard<std::mutex> lock(s.routineLock);
            s.activeIsCycle = false;
            StartRoutine(s, [origin, status, ct, before]
            {
                bool ok = origin->Init(ct);
                if (ok)
                {
                    McDataManager::RunData()->UnsafePause = false;
                    McDataManager::SaveRunningData();
                    status->SetMessage("Init done — machine at standby");
                }
                if (status->State() == EMcState::Homing)
                    status->SetState(ok ? (before == EMcState::Paused ? EMcState::Paused : EMcState::Idle) : EMcState::Paused);
            });
        }

        // ───────────────────────── trạng thái & ngõ ra ─────────────────────────

        static void UpdateState(State& s)
        {
            McStatus& status = s.status;
            status.SetOrgOk(s.motion.AllHomed());
            status.SetVisionConnected(s.vision != nullptr && s.vision->IsConnected());

            // EMG và ALARM đè lên mọi trạng thái khác.
            if (AlarmManager::IsActive(AlarmCode::EmergencyStop))
            {
                if (status.State() != EMcState::Emg)
                {
                    status.SetState(EMcState::Emg);
                    McDataManager::SaveRunningData();
                    Log("Entered EMG state", LogLevel::Error);
                }
                return;
            }

            if (AlarmManager::HasError())
            {
                if (status.State() != EMcState::Alarm)
                {
                    status.SetState(EMcState::Alarm);
                    McDataManager::SaveRunningData();
                }
                return;
            }

            // Thoát EMG/ALARM cần bấm RESET — không tự chạy lại khi lỗi chập chờn tự hết.
            if (status.State() == EMcState::Emg || status.State() == EMcState::Alarm) return;

            if (status.State() == EMcState::Init && s.motion.AllConnected())
                status.SetState(EMcState::NotHomed);

            if (status.State() == EMcState::Idle && !status.OrgOk())
                status.SetState(EMcState::NotHomed);
        }

        static void UpdateOutputs(State& s)
        {
            McStatus& status = s.status;
            IIoDevice& io = *s.io;
            ETowerLamp lamp = ComputeLamp();
            status.SetTowerLamp(lamp);

            long long ms = s.clock.ElapsedMilliseconds();
            bool blinkPhase = (ms / 250) % 2 == 0;   // 2 Hz
            bool blinkSlow = (ms / 500) % 2 == 0;    // 1 Hz

            bool red = lamp == ETowerLamp::Red || (lamp == ETowerLamp::RedBlink && blinkPhase);
            bool yellow = lamp == ETowerLamp::Yellow || (lamp == ETowerLamp::YellowBlink && blinkSlow);
            bool green = lamp == ETowerLamp::Green || (lamp == ETowerLamp::GreenBlink && blinkSlow);

            io.SetDo(IoMap::DoLampRed, red);
            io.SetDo(IoMap::DoLampYellow, yellow);
            io.SetDo(IoMap::DoLampGreen, green);

            bool wantBuzzer = AlarmManager::HasError();
            if (!wantBuzzer) status.SetBuzzerMuted(false);   // alarm mới sẽ lại kêu
            status.SetBuzzer(wantBuzzer && !status.BuzzerMuted());
            io.SetDo(IoMap::DoBuzzer, status.Buzzer());

            // Đèn nút START: sáng im khi đang chạy, nhấp nháy 1 Hz khi Armed / tạm dừng / chờ thay tray (chờ bấm START),
            // tắt khi đứng.
            EMcState st = status.State();
            bool running = st == EMcState::Running || st == EMcState::Pausing;
            bool waitStart = st == EMcState::Armed || st == EMcState::Held || s.autoR->WaitingTray;
            io.SetDo(IoMap::DoLampStartBtn, running || (waitStart && blinkSlow));
            // Đèn STOP: sáng khi trục KHÔNG chạy — kể cả lúc chờ người vận hành thay tray.
            io.SetDo(IoMap::DoLampStopBtn, !running);
            // Đèn RESET: sáng khi còn alarm.
            io.SetDo(IoMap::DoLampResetBtn, AlarmManager::ActiveCount() > 0);
        }

        /// Chờ routine đang chạy thoát rồi JOIN luồng của nó.
        static void JoinRoutine(State& s, int waitMs)
        {
            std::shared_ptr<std::atomic<bool>> done;
            { std::lock_guard<std::mutex> lock(s.routineLock); done = s.routineDone; }
            base::Stopwatch sw;
            while (done && !done->load() && sw.ElapsedMilliseconds() < waitMs) base::Delay(5);
            std::lock_guard<std::mutex> lock(s.routineLock);
            if (s.routine.joinable()) s.routine.join();
        }
    };

    // ───────────────────────── accessor ─────────────────────────

    McStatus& McControl::Status() { return St().status; }
    MotionConnect& McControl::Motion() { return St().motion; }
    std::shared_ptr<IIoDevice> McControl::Io() { auto& s = St(); std::lock_guard<std::mutex> l(s.devMutex); return s.io; }
    std::shared_ptr<VisionConnect> McControl::Vision() { auto& s = St(); std::lock_guard<std::mutex> l(s.devMutex); return s.vision; }
    std::shared_ptr<SafetyMonitor> McControl::Safety() { auto& s = St(); std::lock_guard<std::mutex> l(s.devMutex); return s.safety; }
    std::shared_ptr<McFunction> McControl::Func() { auto& s = St(); std::lock_guard<std::mutex> l(s.devMutex); return s.func; }
    std::shared_ptr<Routine_Auto> McControl::AutoRoutine() { auto& s = St(); std::lock_guard<std::mutex> l(s.devMutex); return s.autoR; }
    std::shared_ptr<Routine_Calib> McControl::Calib() { auto& s = St(); std::lock_guard<std::mutex> l(s.devMutex); return s.calib; }
    double McControl::CurrentCycleSec() { auto a = AutoRoutine(); return a ? a->CurrentCycleSec() : 0; }
    int McControl::CurrentCycleProducts() { auto a = AutoRoutine(); return a ? a->CurrentCycleProducts() : 0; }
    base::Event<std::string>& McControl::AutoModeRefused() { return St().autoModeRefused; }
    bool McControl::IsInitialized() { return St().initialized.load(); }
    int McControl::VisionLoadCount() { return St().visionLoadCount.load(); }

    void McControl::RequestSetMode(EMcMode mode) { St().requestedMode = (int)mode; }
    void McControl::RequestStart() { St().reqStart.exchange(1); }
    void McControl::RequestStop() { St().reqStop.exchange(1); }
    void McControl::RequestPause() { St().reqPause.exchange(1); }
    void McControl::RequestReset() { St().reqReset.exchange(1); }
    void McControl::RequestOrigin() { St().reqOrigin.exchange(1); }
    void McControl::RequestInit() { St().reqInit.exchange(1); }
    void McControl::RequestMuteBuzzer() { St().reqMute.exchange(1); }
    void McControl::RequestArm() { St().reqArm.exchange(1); }

    bool McControl::RoutineBusy()
    {
        State& s = St();
        std::lock_guard<std::mutex> lock(s.routineLock);
        return s.routineDone && !s.routineDone->load();
    }

    std::string McControl::ResetTrays()
    {
        State& s = St();
        if (RoutineBusy() && !(s.autoR && s.autoR->WaitingTray)) return "cycle in progress — stop first";
        if (s.autoR && s.autoR->WaitingTray)
        {
            s.autoR->TrayStartRequest = true;   // chu trình tự đặt lại và chạy tiếp
            return {};
        }
        auto run = McDataManager::RunData();
        run->ResetTrays();
        McDataManager::SaveRunningData();
        s.status.SetMessage("Tray progress reset — next cycle starts from row 1 of tray 1");
        Log("Tray progress reset by operator", LogLevel::Info);
        return {};
    }

    ETowerLamp McControl::ComputeLamp()
    {
        switch (St().status.State())
        {
            case EMcState::Emg:          return ETowerLamp::RedBlink;
            case EMcState::Alarm:        return ETowerLamp::Red;
            case EMcState::Running:      return ETowerLamp::Green;
            case EMcState::Pausing:      return ETowerLamp::Green;
            case EMcState::Paused:       return ETowerLamp::Yellow;
            case EMcState::Held:         return ETowerLamp::Yellow;
            case EMcState::WaitOperator: return ETowerLamp::YellowBlink;
            case EMcState::Armed:        return ETowerLamp::GreenBlink;
            case EMcState::Idle:         return AlarmManager::HasWarning() ? ETowerLamp::YellowBlink : ETowerLamp::Yellow;
            case EMcState::NotHomed:     return ETowerLamp::Yellow;
            case EMcState::Homing:       return ETowerLamp::YellowBlink;
            default:                     return ETowerLamp::Off;
        }
    }

    // ───────────────────────── khởi tạo ─────────────────────────

    void McControl::Init()
    {
        State& s = St();
        if (s.running) return;   // đang chạy rồi — Shutdown() trước khi Init() lại
        auto sys = McDataManager::SysData();
        auto sysFn = [] { return McDataManager::SysData(); };

        // Khởi tạo lại sau Shutdown(): không mang yêu cầu/chế độ của phiên trước sang.
        for (auto* r : { &s.reqStart, &s.reqStop, &s.reqPause, &s.reqReset, &s.reqOrigin, &s.reqInit, &s.reqMute, &s.reqArm }) r->store(0);
        s.requestedMode = (int)EMcMode::Manual;
        s.status.SetMode(EMcMode::Manual);
        s.status.SetOrgOk(false);

        auto model = McDataManager::Model();
        s.status.SetSimulation(sys->SimulationMode);
        s.status.SetModelName(model ? model->Name : std::string("(not loaded)"));
        s.status.SetState(EMcState::Init);

        s.motion.Init(*sys);
        s.motion.SetZSafe(sys->ZSafe);
        s.motion.SetZSafeInterlock(sys->ZSafeInterlock);

        // Remote IO đi chung bus EtherCAT với 3 trục (Motion.Bus) — không có kết nối riêng.
        std::shared_ptr<IIoDevice> io;
        if (sys->SimulationMode) io = std::make_shared<FakeIo>();
        else io = std::make_shared<EtherCatIoDevice>([] { return McDataManager::SysData()->IoModule; }, s.motion.Bus());
        // VisionConnect tự giữ FakeVision khi mô phỏng VÀ chọn vision giả (hoặc app không đăng ký VisionMaster thật) —
        // đã gắn tên Flow/biến; Connect() dựng kho biến giả.
        auto vision = std::make_shared<VisionConnect>(sys);
        if (auto fake = vision->Fake())
        {
            // Như bản C#: đọc lại McDataManager::SysData() mỗi lần (VisionConnect gắn sẵn bản chụp lúc tạo).
            fake->SetNames([] { return McDataManager::SysData()->VisionOutputs; });
            fake->SetFlowOutputSource([](const std::string& flow) { return VmCycle::FakeFlowOutputs(flow, McDataManager::SysData()->VisionOutputs); });
        }

        auto safety = std::make_shared<SafetyMonitor>(s.motion, s.status, sysFn);
        safety->AttachIo(io);
        safety->SetCurtainBroken([&s] { Impl::HoldForCurtain(s); });

        auto func = std::make_shared<McFunction>(s.motion, *safety, s.status, sysFn);
        func->SetIo(io);
        auto origin = std::make_shared<Routine_Origin>(s.motion, *func, s.status, sysFn);
        auto autoR = std::make_shared<Routine_Auto>(s.motion, *func, s.status, sysFn,
            [] { return std::static_pointer_cast<IVisionDevice>(McControl::Vision()); });
        auto calib = std::make_shared<Routine_Calib>(s.motion, *func, sysFn,
            [] { return std::static_pointer_cast<IVisionDevice>(McControl::Vision()); });
        {
            std::lock_guard<std::mutex> lock(s.devMutex);
            s.io = io; s.vision = vision; s.safety = safety; s.func = func; s.origin = origin; s.autoR = autoR; s.calib = calib;
        }
        {
            std::lock_guard<std::mutex> lock(s.routineLock);
            s.routineDone.reset(); s.cycleCts.reset(); s.activeIsCycle = false; s.cycleEnding = false;
        }

        s.motion.ConnectAll();
        io->Connect();
        vision->Connect();
        Impl::LoadVisionSolution(s, *sys);

        s.status.SetState(EMcState::NotHomed);
        s.initialized = true;

        Impl::StartLoop(s, sys->ControlTickMs);
        auto bus = s.motion.Bus();
        Log(sys->SimulationMode
                ? std::string("Init complete — SIMULATION MODE, no real hardware control")
                      + (vision->IsSimulation() ? " (simulated vision)" : " (vision: real VisionMaster)")
                : "Init complete — " + (bus ? bus->BusDescription() : std::string("no EtherCAT bus")),
            LogLevel::Info);
    }

    std::string McControl::ReconnectEtherCat()
    {
        State& s = St();
        auto bus = s.motion.Bus();
        if (!bus) return "simulation mode — no EtherCAT bus";
        if (RoutineBusy()) return "cycle in progress — stop fully before reconnecting";
        if (s.status.State() == EMcState::Running || s.status.State() == EMcState::Homing) return "machine running";

        s.motion.StopAll();
        bool ok = bus->Reconnect();
        for (const auto& axis : s.motion.AxisList()) axis->Refresh();
        if (auto io = Io()) io->Connect();

        if (ok)
        {
            for (const auto& axis : s.motion.AxisList()) AlarmManager::ClearConditionAxis(axis->Name(), AxisFault::Disconnected);
            std::string msg = "EtherCAT bus reopened — servo ON and Origin before running";
            s.status.SetMessage(msg);
            Log(msg, LogLevel::Info);
            return {};
        }
        std::string err = bus->LastError();
        return err.empty() ? std::string("cannot open EtherCAT bus") : err;
    }

    std::string McControl::DisconnectEtherCat()
    {
        State& s = St();
        auto bus = s.motion.Bus();
        if (!bus) return "simulation mode — no EtherCAT bus";
        if (RoutineBusy()) return "cycle in progress — stop fully before disconnecting bus";
        if (s.status.State() == EMcState::Running || s.status.State() == EMcState::Homing) return "machine running";

        s.motion.StopAll();
        try
        {
            std::vector<std::function<bool()>> jobs;
            for (const auto& a : s.motion.AxisList())
                if (a->ServoOn()) jobs.push_back([a] { return a->SetServoOn(false); });
            if (!jobs.empty()) base::WhenAll(std::move(jobs));
        }
        catch (...) { }
        bus->Disconnect();
        for (const auto& axis : s.motion.AxisList()) axis->Refresh();
        std::string msg = "EtherCAT bus disconnected — press Connect All Axis to reopen";
        s.status.SetMessage(msg);
        Log(msg, LogLevel::Warning);
        return {};
    }

    std::string McControl::ReloadVisionSolution()
    {
        if (RoutineBusy()) return "Cycle in progress — stop cycle before reloading solution";
        if (!Vision()) return "vision not initialized";
        Impl::LoadVisionSolution(St(), *McDataManager::SysData());
        return {};
    }

    GlobalVarSyncReport McControl::SyncVisionGlobalVariables(bool apply)
    {
        auto vision = Vision();
        if (!vision || !vision->IsSolutionLoaded())
            return GlobalVarSyncReport::Fail("Vision solution not loaded");
        if (apply && RoutineBusy())
            return GlobalVarSyncReport::Fail("cycle running — stop fully before creating variables");

        auto specs = VisionGlobalVarCatalog::Build(McDataManager::SysData().get());
        auto report = vision->SyncGlobalVariables(specs, apply);

        bool bad = !report.Error.empty() || report.Count(EGlobalVarState::Failed) > 0 ||
                   report.Count(EGlobalVarState::WrongType) > 0 || report.Count(EGlobalVarState::InvalidName) > 0;
        Log((apply ? "Create global variables: " : "Check global variables: ") + ReplaceNewLines(report.ToText()),
            bad ? LogLevel::Warning : LogLevel::Info, "Vision");
        return report;
    }

    VmOutCheckReport McControl::CheckVisionFlowOutputs()
    {
        auto vision = Vision();
        if (!vision || !vision->IsSolutionLoaded())
            return VmOutCheckReport::Fail("Vision solution not loaded");

        VisionOutputNames names = McDataManager::SysData()->VisionOutputs;
        auto scripts = VmScriptContract::Build(names);

        VmOutCheckReport report;
        for (const auto& expect : VmCycle::ExpectedFlowOutputs(names))
        {
            VmFlowOutputs actual = vision->ReadFlowOutputs(expect.Flow);
            VmOutCheck::CompareFlow(expect, &actual, report);

            for (const auto& sc : scripts)
                if (sc.Flow == expect.Flow) VmOutCheck::CompareScript(sc, &actual, report);
        }

        Log("Check Flow outputs: " + ReplaceNewLines(report.ToText()),
            report.Clean() ? LogLevel::Info : LogLevel::Warning, "Vision");
        return report;
    }

    void McControl::Shutdown()
    {
        State& s = St();
        auto safety = Safety();
        auto io = Io();
        auto vision = Vision();
        s.initialized = false;

        // Huỷ AbortToken TRƯỚC KHI đóng bus — mọi routine và lệnh tay đều observe chung token này. Đợi
        // routine kết thúc CÓ GIỚI HẠN: sau khi huỷ, các vòng chờ vị trí thoát gần như ngay.
        if (safety) safety->RequestAbort("Application closing");
        Impl::JoinRoutine(s, 3000);

        s.running = false;
        if (s.watchdog.joinable()) { s.watchdog.request_stop(); s.watchdog.join(); }
        if (s.loop.joinable()) { s.loop.request_stop(); s.loop.join(); }

        // Vòng quét có thể vừa kịp khởi động một routine mới (và cấp token mới) trước khi dừng.
        if (safety) safety->RequestAbort("Application closing");
        Impl::JoinRoutine(s, 3000);

        s.motion.StopAll();
        McDataManager::SaveRunningData();

        // Tắt servo trước khi đóng bus: đóng bus trong lúc Operation Enabled thì driver tự báo lỗi
        // watchdog PDO ở lần bật sau.
        if (!s.motion.Simulation())
        {
            try
            {
                std::vector<std::function<bool()>> jobs;
                for (const auto& a : s.motion.AxisList()) jobs.push_back([a] { return a->SetServoOn(false); });
                if (!jobs.empty()) base::WhenAll(std::move(jobs));
            }
            catch (...) { }
        }

        // Việc nền còn dở (nạp solution vision, reset alarm trục) phải xong trước khi đóng thiết bị.
        if (!base::WaitBackgroundIdle(10000))
            Log("Shutdown: background tasks still running after 10 s", LogLevel::Warning);

        s.motion.DisconnectAll();
        if (io) io->Disconnect();
        if (vision) vision->Disconnect();
        s.motion.DisposeBus();
    }
}
