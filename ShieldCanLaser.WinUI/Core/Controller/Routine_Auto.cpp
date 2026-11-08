#include "Controller/Routine_Auto.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

#include "Base/Async.h"
#include "Communication/VisionConnect.h"
#include "Data/McDataManager.h"
#include "Infor/AlarmManager.h"
#include "Infor/AxisId.h"
#include "Utility/Log/LogWorker.h"

namespace scs
{
    using Clock = std::chrono::steady_clock;

    namespace
    {
        /// <summary>Không chạy tiếp được vì thiếu dữ liệu (điểm teach, tray…) — Run bắt, báo lý do, máy về Paused.</summary>
        struct AutoAbort : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        /// <summary>Chờ tối đa (ms) cảm biến chân không lên sau khi nhấc.</summary>
        constexpr int VacuumSensorWaitMs = 200;

        void Log(const std::string& msg, LogLevel level = LogLevel::Info)
        {
            LogWorker::Instance().RecordLog(msg, level, "Auto");
        }


        // ── Sơ đồ tray + nozzle cho trang Main ──
        std::mutex s_viewMutex;
        Routine_Auto::TrayView s_trays;
        std::array<Routine_Auto::NozzleView, Routine_Auto::NozzleCount + 1> s_nozzles;

        constexpr int Cols = Routine_Auto::NozzleCount;

        /// <summary>Ô thuộc phạm vi chạy: chưa xong, và đang chọn ô thì phải là ô chọn.</summary>
        bool InScope(const TrayCells& book, int cell, bool selecting)
        {
            return !book.IsDone(cell) && (!selecting || book.IsSelected(cell));
        }

        /// <summary>Dựng sơ đồ từ các ô đã chạy (mở app giữa chừng / vừa đổi kích thước tray): ô đã xong = Skipped.</summary>
        void TraysEnsure(int rows, const RunningData& run)
        {
            std::lock_guard<std::mutex> l(s_viewMutex);
            const size_t size = (size_t)std::max(0, rows) * Cols;
            for (int t = 0; t < Routine_Auto::TrayCount; ++t)
            {
                auto& view = s_trays.Trays[(size_t)t];
                if (view.size() == size) continue;
                view.assign(size, (int)ECellState::Loaded);
                for (size_t i = 0; i < size; ++i)
                    if (run.Trays[(size_t)t].IsDone((int)i)) view[i] = (int)ECellState::Skipped;
            }
        }

        void TraysFill(int rows)
        {
            std::lock_guard<std::mutex> l(s_viewMutex);
            const size_t size = (size_t)std::max(0, rows) * Cols;
            for (int t = 0; t < Routine_Auto::TrayCount; ++t)
                s_trays.Trays[(size_t)t].assign(size, (int)ECellState::Loaded);
        }

        void CellSet(int tray, int row, int col, ECellState state)
        {
            std::lock_guard<std::mutex> l(s_viewMutex);
            if (tray < 0 || tray >= Routine_Auto::TrayCount) return;
            auto& view = s_trays.Trays[(size_t)tray];
            size_t i = (size_t)row * Cols + (size_t)col;
            if (i < view.size()) view[i] = (int)state;
        }

        void NozzleSet(int k, ECellState state, const std::string& text = {})
        {
            std::lock_guard<std::mutex> l(s_viewMutex);
            if (k < 1 || k > Routine_Auto::NozzleCount) return;
            s_nozzles[(size_t)k].State = (int)state;
            s_nozzles[(size_t)k].Text = text;
        }

        void NozzleResult(int k, bool has, double result)
        {
            std::lock_guard<std::mutex> l(s_viewMutex);
            if (k < 1 || k > Routine_Auto::NozzleCount) return;
            s_nozzles[(size_t)k].HasResult = has;
            s_nozzles[(size_t)k].Result = has ? result : 0;
        }

        void NozzlesClear()
        {
            std::lock_guard<std::mutex> l(s_viewMutex);
            for (auto& n : s_nozzles) n = {};
        }
    }

    // ───────────────────────── đồng hồ cycle ─────────────────────────

    void Routine_Auto::CycleClock::Restart()
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_accum = {}; m_since = Clock::now(); m_running = true;
    }

    void Routine_Auto::CycleClock::Stop()
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (!m_running) return;
        m_accum += Clock::now() - m_since; m_running = false;
    }

    void Routine_Auto::CycleClock::Start()
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (m_running) return;
        m_since = Clock::now(); m_running = true;
    }

    void Routine_Auto::CycleClock::Reset()
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_accum = {}; m_running = false;
    }

    double Routine_Auto::CycleClock::Seconds() const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        auto d = m_accum;
        if (m_running) d += Clock::now() - m_since;
        return std::chrono::duration<double>(d).count();
    }

    Routine_Auto::Routine_Auto(MotionConnect& motion, McFunction& func, McStatus& status,
                               std::function<std::shared_ptr<SystemData>()> sys, std::function<std::shared_ptr<IVisionDevice>()> vision)
        : m_motion(motion), m_func(func), m_status(status), m_sys(std::move(sys)), m_vision(std::move(vision))
    {
    }

    void Routine_Auto::HoldClock(bool hold)
    {
        if (hold) m_cycleClock.Stop();
        else if (m_cycleClock.Seconds() > 0) m_cycleClock.Start();
    }

    // ───────────────────────── vòng chạy ─────────────────────────

    bool Routine_Auto::Run(std::stop_token ct)
    {
        McFunction::MarkCycleFlow();
        struct Cleanup
        {
            Routine_Auto* r;
            ~Cleanup()
            {
                r->WaitingTray = false;
                r->TrayStartRequest = false;
            }
        } cleanup{ this };

        m_model = McDataManager::Model();
        m_cycleClock.Reset();
        SetStep(EAutoState::S00_Ready, "Auto running");

        try
        {
            if (!PrepareStart(ct)) return false;
            auto run = McDataManager::RunData();
            bool selecting = false;   // hàng vừa chạy là từ ô chọn; MarkDone bỏ chọn chúng, hết ô chọn là xong lượt chạy lẻ
            while (true)
            {
                if (PauseRequested) return false;
                if (selecting && !run->HasCellSelection()) return FinishSelection(ct);
                int tray = 0, row = 0;
                if (!NextRow(tray, row))
                {
                    if (!WaitTrays(ct)) return false;
                    continue;
                }
                selecting = run->HasCellSelection();
                if (!RunRow(tray, row, ct)) return false;
            }
        }
        catch (const AutoAbort& ex)
        {
            std::string msg = std::string("Cycle stopped: ") + ex.what();
            m_status.SetMessage(msg);
            Log(msg, LogLevel::Warning);
            return false;
        }
    }

    /// Trạng thái để lại từ lần chạy trước: nozzle còn giữ can thì không biết trả về đâu → dừng, người vận hành gỡ can
    /// và tắt van ở trang Manual. Điểm teach thiếu thì báo ngay, không đợi tới lúc chạy.
    bool Routine_Auto::PrepareStart(std::stop_token)
    {
        for (int k = 1; k <= NozzleCount; ++k)
        {
            if (Io().GetDo(IoMap::DoVacuum(k)))
                throw AutoAbort(std::format("Nozzle {} vacuum still ON (previous cycle interrupted) — remove can, turn vacuum OFF on Manual page, then restart", k));
            if (Io().GetDo(IoMap::VacuumBlower(k)))
                throw AutoAbort(std::format("Nozzle {} blower still ON — turn it OFF on Manual page, then restart", k));
        }

        PickAt(0, 0);
        LaserAt(1);
        if (m_model->Auto.UseVision)
        {
            // Kiểm tra không chạy được thì dừng ngay từ đầu — đừng khắc cả tray rồi mới đánh NG hết.
            CamAt(1);
            if (auto why = m_sys()->VisionOutputs.InspectPathProblem(); !why.empty())
                throw AutoAbort("Vision: " + why + " — or turn off Use vision for this model");
            auto vision = m_vision ? m_vision() : nullptr;
            if (!vision || !vision->IsSolutionLoaded())
                throw AutoAbort("Vision solution not loaded — load it on Setting page, or turn off Use vision for this model");
        }

        auto run = McDataManager::RunData();
        const int rows = std::max(1, m_model->Auto.TrayRows);
        TraysEnsure(rows, *run);
        NozzlesClear();
        // Model vừa giảm số hàng: ô chọn ngoài tray không còn trên sơ đồ, giữ lại thì không bao giờ xong.
        for (auto& tray : run->Trays)
            for (int c : tray.Selected())
                if (c >= rows * Cols) tray.Toggle(c);
        return true;
    }

    /// Hàng đầu tiên còn ô trong phạm vi chạy: đang chọn ô thì chỉ xét các ô chọn, không thì cả tray. Tray vắng thì bỏ qua —
    /// trừ khi nó có ô chọn chưa chạy: dừng báo ngay, không âm thầm bỏ ô người vận hành đã chọn.
    bool Routine_Auto::NextRow(int& tray, int& row) const
    {
        auto run = McDataManager::RunData();
        const int cells = std::max(1, m_model->Auto.TrayRows) * Cols;
        const bool selecting = run->HasCellSelection();
        bool found = false;
        for (int t = 0; t < TrayCount; ++t)
        {
            const bool present = TrayPresent(t);
            const TrayCells& book = run->Trays[(size_t)t];
            for (int i = 0; i < cells; ++i)
            {
                if (!InScope(book, i, selecting)) continue;
                if (selecting && !present)
                    throw AutoAbort(std::format("Tray {} has selected cells but is not detected ({}) — load it or clear the selection",
                                                t + 1, IoMap::DiLabel(IoMap::TrayPresent(t + 1))));
                if (present && !found)
                {
                    tray = t;
                    row = i / Cols;
                    found = true;
                }
            }
        }
        return found;
    }

    bool Routine_Auto::ToStandby(std::stop_token ct)
    {
        auto sys = m_sys();
        if (!m_func.EnsureZSafe(ct)) return false;
        return MoveAxes({ { AxisId::X, sys->GetAxis(AxisId::X)->StandbyPos }, { AxisId::Y, sys->GetAxis(AxisId::Y)->StandbyPos } }, ct);
    }

    /// Xong một chu kỳ của bảng Cycle Time: tray vừa hết ô, hết ô chọn, hoặc chờ thay tray lúc tray bị lấy ra giữa chừng.
    void Routine_Auto::FinishCycle()
    {
        auto run = McDataManager::RunData();
        if (run->CycleStartedAtMs == 0) return;
        run->FinishCycle(RunningData::NowMs());
        McDataManager::SaveRunningData();
        Log(std::format("Cycle done: {} OK in {:.1f} s", run->LastCycleProducts, run->LastCycleSec));
    }

    /// Hết ô chọn: về vị trí chờ, kết thúc chu trình. Muốn chạy tiếp phải bấm RUN rồi START — bấm START theo thói quen ngay
    /// sau lượt lẻ không được chạy cả phần còn lại của tray.
    bool Routine_Auto::FinishSelection(std::stop_token ct)
    {
        FinishCycle();
        if (!ToStandby(ct)) return false;
        SetStep(EAutoState::S00_Ready, (m_lastRowText.empty() ? std::string() : m_lastRowText + " — ") +
                                       "Selected cells done — press RUN, then START for the next cells");
        Log("Selected cells done");
        return true;
    }

    /// Hết hàng của cả hai tray (hoặc không có tray): về vị trí chờ, chờ người vận hành thay tray và bấm START.
    bool Routine_Auto::WaitTrays(std::stop_token ct)
    {
        FinishCycle();
        if (!ToStandby(ct)) return false;

        std::string text = (m_lastRowText.empty() ? std::string() : m_lastRowText + " — ") +
                           "Trays done — replace trays, then press START";
        SetStep(EAutoState::S60_WaitTray, text);
        AlarmManager::RaiseEvent(AlarmCode::TrayEmpty, "Auto", "both trays done");
        EnterWait();
        WaitingTray = true;
        TrayStartRequest = false;

        while (true)
        {
            if (PauseRequested) { WaitingTray = false; return false; }
            if (TrayStartRequest.exchange(false))
            {
                bool any = false;
                for (int t = 0; t < TrayCount; ++t) any = any || TrayPresent(t);
                if (!any)
                {
                    m_status.SetMessage(std::format("START rejected: no tray detected ({}/{})", IoMap::DiLabel(IoMap::TrayPresent(1)), IoMap::DiLabel(IoMap::TrayPresent(2))));
                }
                else
                {
                    auto run = McDataManager::RunData();
                    run->ResetTrays();
                    McDataManager::SaveRunningData();
                    TraysFill(std::max(1, m_model->Auto.TrayRows));
                    m_lastRowText.clear();
                    WaitingTray = false;
                    LeaveWait();
                    Log(run->HasCellSelection() ? "Trays replaced — running selected cells" : "Trays replaced — starting from row 1");
                    return true;
                }
            }
            base::Delay(20, ct);
        }
    }

    // ───────────────────────── một hàng ─────────────────────────

    bool Routine_Auto::RunRow(int tray, int row, std::stop_token ct)
    {
        Row r;
        r.Tray = tray;
        r.Index = row;
        r.Result.fill((int)ECellState::Empty);
        {
            auto run = McDataManager::RunData();
            const bool selecting = run->HasCellSelection();
            for (int k = 1; k <= NozzleCount; ++k) r.Active[k] = InScope(run->Trays[(size_t)tray], row * Cols + k - 1, selecting);
            run->StartCycleClock(RunningData::NowMs());
        }
        m_cycleClock.Restart();
        Log(std::format("Row start: tray {} row {} ({} cells)", tray + 1, row + 1, std::count(r.Active.begin(), r.Active.end(), true)));
        for (int k = 1; k <= NozzleCount; ++k) if (r.Active[k]) CellSet(tray, row, k - 1, ECellState::Processing);

        if (!PickRow(r, ct)) return false;
        if (PauseRequested)
        {
            // Dừng ở checkpoint nhưng không bao giờ để can trên nozzle: trả hàng về rồi mới dừng — hàng này chưa khắc,
            // lần START sau chạy lại chính hàng này.
            Log("Stop requested after pick — returning row to tray, row will be rerun", LogLevel::Warning);
            if (!PlaceRow(r, ct)) return false;
            for (int k = 1; k <= NozzleCount; ++k) if (r.Active[k]) CellSet(tray, row, k - 1, ECellState::Loaded);
            NozzlesClear();
            return false;
        }
        // Từ đây can đã khắc: STOP chỉ có hiệu lực sau khi kiểm tra và trả hàng (khắc lại là khắc đè).
        if (!Laser(r, ct)) return false;
        if (!Inspect(r, ct)) return false;
        if (!PlaceRow(r, ct)) return false;
        m_cycleClock.Stop();
        Finish(r);
        return !PauseRequested;
    }

    /// Gắp hàng: Y tới hàng, X/Z tới điểm gắp, Z hạ hai chặng, van của các ô trong phạm vi bật, chờ, Z nâng, đọc cảm biến chân
    /// không của các ô đó. Nozzle ngoài phạm vi vẫn hạ theo (Z chung), không bật van, không tính hút hụt.
    bool Routine_Auto::PickRow(Row& r, std::stop_token ct)
    {
        SetStep(EAutoState::S20_PickRow, std::format("Picking row {} of tray {}", r.Index + 1, r.Tray + 1));
        auto p = PickAt(r.Tray, r.Index);
        double x = Coord(*p, AxisId::X), y = Coord(*p, AxisId::Y), z = Coord(*p, AxisId::Z);
        const auto& ap = m_model->Auto;

        if (!m_func.EnsureZSafe(ct)) return false;
        if (!MoveAxes({ { AxisId::X, x }, { AxisId::Y, y } }, ct)) return false;
        if (!m_func.MoveZDown(AxisId::Z, z, z - ap.PickSlowLength, ct)) return false;

        for (int k = 1; k <= NozzleCount; ++k) if (r.Active[k]) Io().SetDo(IoMap::DoVacuum(k), true);
        base::Delay(ap.VacuumOnMs, ct);
        if (!m_func.EnsureZSafe(ct)) return false;

        base::Stopwatch sw;
        auto allUp = [&] {
            for (int k = 1; k <= NozzleCount; ++k) if (r.Active[k] && !Io().GetDi(IoMap::VacuumSensor(k))) return false;
            return true;
        };
        while (!allUp() && sw.ElapsedMilliseconds() < VacuumSensorWaitMs) base::Delay(10, ct);

        int active = 0, missed = 0;
        for (int k = 1; k <= NozzleCount; ++k)
        {
            if (!r.Active[k]) continue;
            ++active;
            r.Holding[k] = Io().GetDi(IoMap::VacuumSensor(k));
            if (r.Holding[k])
            {
                r.Result[k] = (int)ECellState::Loaded;
                NozzleSet(k, ECellState::Loaded, "holding");
            }
            else
            {
                Io().SetDo(IoMap::DoVacuum(k), false);
                r.Result[k] = (int)ECellState::Ng;
                CellSet(r.Tray, r.Index, k - 1, ECellState::Ng);
                NozzleSet(k, ECellState::Empty, "pick missed");
                ++missed;
            }
        }
        if (missed > 0)
            AlarmManager::RaiseEvent(AlarmCode::PickFailed, "Auto",
                std::format("{} nozzle(s) missed on tray {} row {} — cells set NG", missed, r.Tray + 1, r.Index + 1));
        if (missed == active)
            Log(std::format("Row {} of tray {}: no can picked — empty cells?", r.Index + 1, r.Tray + 1), LogLevel::Warning);
        return true;
    }

    /// Khắc từng nozzle đang giữ can: X/Z tới điểm Laser(k) + Trim, DO kích, chờ MarkTimeMs. Không bù độ lệch can.
    bool Routine_Auto::Laser(Row& r, std::stop_token ct)
    {
        SetStep(EAutoState::S30_Laser, "Laser marking each nozzle");
        for (int k = 1; k <= NozzleCount; ++k)
        {
            if (!r.Holding[k] || r.Result[k] == (int)ECellState::Ng) continue;
            auto p = LaserAt(k);
            const auto& trim = m_model->Trim.Nozzle(k);
            const double x = Coord(*p, AxisId::X) + trim.X, z = Coord(*p, AxisId::Z) + trim.Z;

            NozzleSet(k, ECellState::Processing, std::format("marking at X {:.3f}, Z {:.3f}", x, z));
            if (!m_func.EnsureZSafe(ct)) return false;
            if (!MoveAxes({ { AxisId::X, x } }, ct)) return false;
            if (!MoveZTo(z, ct)) return false;
            m_func.WaitSettle(ct);
            Log(std::format("Nozzle {}: mark at X {:.3f}, Z {:.3f}{}", k, x, z, m_model->Auto.UseLaser ? "" : " (dry run, laser off)"));
            if (m_model->Auto.UseLaser) Mark(ct);
            r.Marked[k] = true;
            NozzleSet(k, ECellState::Loaded, "marked");
        }
        return true;
    }

    /// Kiểm tra từng can vừa khắc: X/Z tới điểm CAM(k), chờ ổn định, chạy Flow, đọc Result của Group. Result = 1 → OK,
    /// khác 1 → NG. Không đọc được (Flow lỗi, quá giờ, thiếu Result) → NG kèm cảnh báo 5006. Vision tắt → can đã khắc là OK.
    bool Routine_Auto::Inspect(Row& r, std::stop_token ct)
    {
        if (!m_model->Auto.UseVision)
        {
            for (int k = 1; k <= NozzleCount; ++k)
            {
                if (!r.Marked[k]) continue;
                r.Result[k] = (int)ECellState::Ok;
                NozzleSet(k, ECellState::Ok, "marked (vision off)");
            }
            return true;
        }
        if (std::none_of(r.Marked.begin(), r.Marked.end(), [](bool m) { return m; })) return true;
        SetStep(EAutoState::S40_Inspect, "CAM inspecting each marked can");

        Io().SetDo(IoMap::DoLightCam, true);
        struct LightOff { IIoDevice& io; ~LightOff() { io.SetDo(IoMap::DoLightCam, false); } } lightOff{ Io() };
        bool first = true;
        for (int k = 1; k <= NozzleCount; ++k)
        {
            if (!r.Marked[k]) continue;
            NozzleSet(k, ECellState::Processing, "inspecting");
            auto p = CamAt(k);
            if (!m_func.EnsureZSafe(ct)) return false;
            if (!MoveAxes({ { AxisId::X, Coord(*p, AxisId::X) } }, ct)) return false;
            if (p->Has(AxisId::Z) && !MoveZTo(p->Get(AxisId::Z), ct)) return false;
            m_func.WaitSettle(ct);
            if (first) { base::Delay(m_sys()->LampSettleMs, ct); first = false; }

            auto read = InspectHere(k, ct);
            NozzleResult(k, read.HasResult, read.Result);
            if (read.Pass)
            {
                r.Result[k] = (int)ECellState::Ok;
                NozzleSet(k, ECellState::Ok, "inspected OK");
                continue;
            }
            r.Result[k] = (int)ECellState::Ng;
            CellSet(r.Tray, r.Index, k - 1, ECellState::Ng);
            NozzleSet(k, ECellState::Ng, read.CallOk && read.HasResult ? std::string("inspected NG") : "vision: " + read.Describe());
            if (!read.CallOk || !read.HasResult)
                AlarmManager::RaiseEvent(AlarmCode::VisionNg, "Auto", std::format("nozzle {}: {}", k, read.Describe()));
        }
        return true;
    }

    /// Trả can về đúng ô: Y/X/Z tới điểm gắp, Z hạ, van tắt, chờ, Z nâng.
    bool Routine_Auto::PlaceRow(Row& r, std::stop_token ct)
    {
        SetStep(EAutoState::S50_PlaceRow, std::format("Returning row {} to tray {}", r.Index + 1, r.Tray + 1));
        auto p = PickAt(r.Tray, r.Index);
        double x = Coord(*p, AxisId::X), y = Coord(*p, AxisId::Y), z = Coord(*p, AxisId::Z);
        const auto& ap = m_model->Auto;

        if (!m_func.EnsureZSafe(ct)) return false;
        if (!MoveAxes({ { AxisId::X, x }, { AxisId::Y, y } }, ct)) return false;
        if (!m_func.MoveZDown(AxisId::Z, z, z - ap.PickSlowLength, ct)) return false;
        for (int k = 1; k <= NozzleCount; ++k) Io().SetDo(IoMap::DoVacuum(k), false);
        {
            // Thổi nhả can trong suốt VacuumOffMs, tắt trước khi Z nâng. Chỉ nozzle đang giữ can: nozzle hút hụt mà thổi
            // thì hất văng can còn nằm trong ô.
            struct BlowOff
            {
                IIoDevice& io;
                ~BlowOff() { for (int k = 1; k <= IoMap::NozzleCount; ++k) io.SetDo(IoMap::VacuumBlower(k), false); }
            } blowOff{ Io() };
            for (int k = 1; k <= NozzleCount; ++k) if (r.Holding[k]) Io().SetDo(IoMap::VacuumBlower(k), true);
            base::Delay(ap.VacuumOffMs, ct);
        }
        if (!m_func.EnsureZSafe(ct)) return false;

        for (int k = 1; k <= NozzleCount; ++k)
        {
            if (!r.Holding[k]) continue;
            r.Holding[k] = false;
            CellSet(r.Tray, r.Index, k - 1, r.Result[k] == (int)ECellState::Ok ? ECellState::Ok : ECellState::Ng);
            NozzleSet(k, ECellState::Empty, r.Result[k] == (int)ECellState::Ok ? "returned (OK)" : "returned (NG)");
        }
        return true;
    }

    void Routine_Auto::Finish(const Row& r)
    {
        auto run = McDataManager::RunData();
        int ok = 0, ng = 0;
        std::vector<int> result;
        for (int k = 1; k <= NozzleCount; ++k)
        {
            result.push_back(r.Result[k]);
            if (!r.Active[k]) continue;
            if (r.Result[k] == (int)ECellState::Ok) ++ok;
            else ++ng;
            run->Trays[(size_t)r.Tray].MarkDone(r.Index * Cols + k - 1);
        }

        run->LastRowResult = result;
        run->LastRowTray = r.Tray + 1;
        run->LastRowIndex = r.Index + 1;
        run->CountOk += ok;
        run->CountNg += ng;
        run->CountTotal += ok + ng;
        run->ConsecutiveNg = ng > 0 ? run->ConsecutiveNg + ng : 0;
        run->CycleOk += ok;
        const int cells = std::max(1, m_model->Auto.TrayRows) * Cols;
        if (run->Trays[(size_t)r.Tray].DoneCount(cells) >= cells) FinishCycle();
        const double sec = m_cycleClock.Seconds();
        McDataManager::SaveRunningData();
        m_cycleClock.Reset();

        m_lastRowText = std::format("Tray {} row {} done: {} OK, {} NG ({:.1f} s)", r.Tray + 1, r.Index + 1, ok, ng, sec);
        m_status.SetMessage(m_lastRowText);
        Log(m_lastRowText, ng > 0 ? LogLevel::Warning : LogLevel::Info);
    }

    // ───────────────────────── vision / laser ─────────────────────────

    VmInspectRead Routine_Auto::InspectHere(int nozzle, std::stop_token ct)
    {
        VmInspectRead read;
        auto vision = m_vision ? m_vision() : nullptr;
        const auto& names = m_sys()->VisionOutputs;
        if (!vision) read.CallError = "vision device not available";
        else if (auto why = names.InspectPathProblem(); !why.empty()) read.CallError = why;
        else read = VmCycle::ReadInspect(vision->CallFlow(VmCycle::InspectRequest(names, nozzle), ct));
        Log(std::format("Inspect N{} ({}): {}", nozzle, names.InspectPath, read.Describe()), read.Pass ? LogLevel::Info : LogLevel::Warning);
        return read;
    }

    void Routine_Auto::Mark(std::stop_token ct)
    {
        auto sys = m_sys();
        int hold = std::max(m_model->Auto.MarkTimeMs, sys->LaserPulseMinMs);
        // Lệnh khắc không bị cắt ngang bởi STOP (bit RUN) — chỉ EMG/alarm (ct) mới cắt.
        m_func.LaserPulse(hold, ct);
    }

    // ───────────────────────── tiện ích ─────────────────────────

    std::shared_ptr<ModelData> Routine_Auto::Model() const
    {
        return m_model ? m_model : McDataManager::Model();
    }

    std::shared_ptr<TeachPosition> Routine_Auto::PickAt(int tray, int row) const
    {
        std::string why;
        auto model = Model();
        auto p = model ? model->PickPoint(tray + 1, row + 1, why) : nullptr;
        if (!p) throw AutoAbort(model ? why : std::string("No model loaded"));
        for (const char* axis : { AxisId::X, AxisId::Y, AxisId::Z }) Coord(*p, axis);
        return p;
    }

    std::shared_ptr<TeachPosition> Routine_Auto::CamAt(int nozzle) const
    {
        std::string why;
        auto model = Model();
        auto p = model ? model->CamPoint(nozzle, why) : nullptr;
        if (!p) throw AutoAbort(model ? why : std::string("No model loaded"));
        Coord(*p, AxisId::X);
        return p;
    }

    std::shared_ptr<TeachPosition> Routine_Auto::LaserAt(int nozzle) const
    {
        std::string why;
        auto model = Model();
        auto p = model ? model->LaserPoint(nozzle, why) : nullptr;
        if (!p) throw AutoAbort(model ? why : std::string("No model loaded"));
        Coord(*p, AxisId::X);
        Coord(*p, AxisId::Z);
        return p;
    }

    double Routine_Auto::Coord(const TeachPosition& p, const char* axis)
    {
        if (!p.Has(axis)) throw AutoAbort(std::format("Teach point '{}' missing axis {} — Teaching page", p.Name, axis));
        return p.Get(axis);
    }

    bool Routine_Auto::MoveAxes(const std::vector<std::pair<std::string, double>>& targets, std::stop_token ct)
    {
        bool inCycle = McFunction::InCycleFlow();
        auto sys = m_sys();
        std::vector<std::function<bool()>> jobs;
        for (const auto& [name, pos] : targets)
        {
            double speed = sys->GetAxis(name)->MoveSpeed;
            std::string axis = name;
            double target = pos;
            jobs.push_back([this, axis, target, speed, ct, inCycle] {
                McFunction::CycleFlowScope scope(inCycle);
                return m_func.Move(axis, target, speed, ct);
            });
        }
        return jobs.empty() || base::WhenAll(std::move(jobs));
    }

    bool Routine_Auto::MoveZTo(double z, std::stop_token ct)
    {
        return m_func.Move(AxisId::Z, z, m_sys()->GetAxis(AxisId::Z)->MoveSpeed, ct);
    }

    bool Routine_Auto::TrayPresent(int tray) const
    {
        if (!m_sys()->HasTraySensors) return true;
        return Io().GetDi(IoMap::TrayPresent(tray + 1));
    }

    void Routine_Auto::SetStep(EAutoState step, const std::string& message)
    {
        m_status.SetAutoStep(step);
        McDataManager::RunData()->AutoState = step;
        m_status.SetMessage(message);
    }

    // Chờ người vận hành = WaitOperator (đèn vàng nháy, STOP huỷ chờ); chạy trục = Running. Chỉ đổi khi đang ở trạng thái
    // kia — McControl có thể vừa đặt Held/Pausing, không được đè lên.
    void Routine_Auto::EnterWait()
    {
        if (m_status.State() == EMcState::Running) m_status.SetState(EMcState::WaitOperator);
    }

    void Routine_Auto::LeaveWait()
    {
        if (m_status.State() == EMcState::WaitOperator) m_status.SetState(EMcState::Running);
    }

    IIoDevice& Routine_Auto::Io() const
    {
        auto io = m_func.Io();
        if (!io) throw AutoAbort("Remote IO not available");
        return *io;
    }

    void Routine_Auto::ClearTrayView()
    {
        std::lock_guard<std::mutex> l(s_viewMutex);
        s_trays = {};
    }

    Routine_Auto::TrayView Routine_Auto::Trays()
    {
        std::lock_guard<std::mutex> l(s_viewMutex);
        return s_trays;
    }

    std::array<Routine_Auto::NozzleView, Routine_Auto::NozzleCount + 1> Routine_Auto::Nozzles()
    {
        std::lock_guard<std::mutex> l(s_viewMutex);
        return s_nozzles;
    }

    std::string Routine_Auto::ProgressText()
    {
        auto run = McDataManager::RunData();
        auto model = McDataManager::Model();
        int cells = model ? std::max(0, model->Auto.TrayRows) * Cols : 0;
        std::string s = std::format("Tray 1: {}/{} cells · Tray 2: {}/{} cells done", run->Trays[0].DoneCount(cells), cells, run->Trays[1].DoneCount(cells), cells);
        if (!run->LastRowResult.empty())
        {
            s += std::format("\nLast row (tray {} row {}):", run->LastRowTray, run->LastRowIndex);
            for (size_t i = 0; i < run->LastRowResult.size(); ++i)
                s += std::format(" N{}·{}", i + 1, run->LastRowResult[i] == (int)ECellState::Ok ? "OK" : run->LastRowResult[i] == (int)ECellState::Ng ? "NG" : "—");
        }
        return s;
    }
}
