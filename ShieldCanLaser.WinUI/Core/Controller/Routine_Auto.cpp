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

        /// <summary>Dựng sơ đồ từ số hàng đã chạy (mở app giữa chừng / vừa đổi kích thước tray): hàng đã xong = Skipped.</summary>
        void TraysEnsure(int rows, const int* rowsDone, const bool* present)
        {
            std::lock_guard<std::mutex> l(s_viewMutex);
            const size_t size = (size_t)std::max(0, rows) * Cols;
            for (int t = 0; t < Routine_Auto::TrayCount; ++t)
            {
                auto& view = s_trays.Trays[(size_t)t];
                if (view.size() == size && (size == 0 || present[t] || view[0] == (int)ECellState::Empty)) continue;
                view.assign(size, present[t] ? (int)ECellState::Loaded : (int)ECellState::Empty);
                if (present[t])
                    for (size_t i = 0; i < size && (int)(i / Cols) < rowsDone[t]; ++i) view[i] = (int)ECellState::Skipped;
            }
        }

        void TraysFill(int rows, const bool* present)
        {
            std::lock_guard<std::mutex> l(s_viewMutex);
            const size_t size = (size_t)std::max(0, rows) * Cols;
            for (int t = 0; t < Routine_Auto::TrayCount; ++t)
                s_trays.Trays[(size_t)t].assign(size, present[t] ? (int)ECellState::Loaded : (int)ECellState::Empty);
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

        void NozzleDeviation(int k, bool has, double dx, double dz)
        {
            std::lock_guard<std::mutex> l(s_viewMutex);
            if (k < 1 || k > Routine_Auto::NozzleCount) return;
            s_nozzles[(size_t)k].HasDeviation = has;
            s_nozzles[(size_t)k].Dx = dx;
            s_nozzles[(size_t)k].Dz = dz;
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

    double Routine_Auto::CurrentCycleSec() const { return m_cycleClock.Seconds(); }

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
        m_cycleDone = 0;
        SetStep(EAutoState::S00_Ready, "Auto running");

        try
        {
            if (!PrepareStart(ct)) return false;
            while (true)
            {
                if (PauseRequested) return false;
                int tray = 0, row = 0;
                if (!NextRow(tray, row))
                {
                    if (!WaitTrays(ct)) return false;
                    continue;
                }
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
            if (Io().GetDo(IoMap::DoVacuum(k)))
                throw AutoAbort(std::format("Nozzle {} vacuum still ON (previous cycle interrupted) — remove can, turn vacuum OFF on Manual page, then restart", k));

        PickAt(0, 0);
        CamAt(1);
        LaserAt(1);

        auto run = McDataManager::RunData();
        bool present[TrayCount];
        for (int t = 0; t < TrayCount; ++t) present[t] = TrayPresent(t);
        TraysEnsure(std::max(1, m_model->Auto.TrayRows), run->TrayRowsDone, present);
        NozzlesClear();
        return true;
    }

    bool Routine_Auto::NextRow(int& tray, int& row) const
    {
        auto run = McDataManager::RunData();
        const int rows = std::max(1, m_model->Auto.TrayRows);
        for (int t = 0; t < TrayCount; ++t)
        {
            if (!TrayPresent(t)) continue;
            if (run->TrayRowsDone[t] < rows)
            {
                tray = t;
                row = std::max(0, run->TrayRowsDone[t]);
                return true;
            }
        }
        return false;
    }

    /// Hết hàng của cả hai tray (hoặc không có tray): về vị trí chờ, chờ người vận hành thay tray và bấm START.
    bool Routine_Auto::WaitTrays(std::stop_token ct)
    {
        auto sys = m_sys();
        if (!m_func.EnsureZSafe(ct)) return false;
        if (!MoveAxes({ { AxisId::X, sys->GetAxis(AxisId::X)->StandbyPos }, { AxisId::Y, sys->GetAxis(AxisId::Y)->StandbyPos } }, ct)) return false;

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
                    m_status.SetMessage("START rejected: no tray detected (X005/X006)");
                }
                else
                {
                    auto run = McDataManager::RunData();
                    run->ResetTrays();
                    McDataManager::SaveRunningData();
                    bool present[TrayCount];
                    for (int t = 0; t < TrayCount; ++t) present[t] = TrayPresent(t);
                    TraysFill(std::max(1, m_model->Auto.TrayRows), present);
                    m_lastRowText.clear();
                    WaitingTray = false;
                    LeaveWait();
                    Log("Trays replaced — starting from row 1");
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
        m_cycleClock.Restart();
        m_cycleDone = 0;
        Log(std::format("Row start: tray {} row {}", tray + 1, row + 1));
        for (int c = 0; c < Cols; ++c) CellSet(tray, row, c, ECellState::Processing);

        if (!PickRow(r, ct)) return false;
        if (PauseRequested)
        {
            // Dừng ở checkpoint nhưng không bao giờ để can trên nozzle: trả hàng về rồi mới dừng — hàng này chưa khắc,
            // lần START sau chạy lại chính hàng này.
            Log("Stop requested after pick — returning row to tray, row will be rerun", LogLevel::Warning);
            if (!PlaceRow(r, ct)) return false;
            for (int c = 0; c < Cols; ++c) CellSet(tray, row, c, ECellState::Loaded);
            NozzlesClear();
            return false;
        }
        if (!Vision(r, ct)) return false;
        if (PauseRequested)
        {
            Log("Stop requested after vision — returning row to tray, row will be rerun", LogLevel::Warning);
            if (!PlaceRow(r, ct)) return false;
            for (int c = 0; c < Cols; ++c) CellSet(tray, row, c, ECellState::Loaded);
            NozzlesClear();
            return false;
        }
        if (!Laser(r, ct)) return false;
        if (!PlaceRow(r, ct)) return false;
        m_cycleClock.Stop();
        Finish(r);
        return !PauseRequested;
    }

    /// Gắp cả hàng: Y tới hàng, X/Z tới điểm gắp, Z hạ hai chặng, 7 van bật, chờ, Z nâng, đọc 7 cảm biến chân không.
    bool Routine_Auto::PickRow(Row& r, std::stop_token ct)
    {
        SetStep(EAutoState::S20_PickRow, std::format("Picking row {} of tray {}", r.Index + 1, r.Tray + 1));
        auto p = PickAt(r.Tray, r.Index);
        double x = Coord(*p, AxisId::X), y = Coord(*p, AxisId::Y), z = Coord(*p, AxisId::Z);
        const auto& ap = m_model->Auto;

        if (!m_func.EnsureZSafe(ct)) return false;
        if (!MoveAxes({ { AxisId::X, x }, { AxisId::Y, y } }, ct)) return false;
        if (!m_func.MoveZDown(AxisId::Z, z, z - ap.PickSlowLength, ct)) return false;

        for (int k = 1; k <= NozzleCount; ++k) Io().SetDo(IoMap::DoVacuum(k), true);
        base::Delay(ap.VacuumOnMs, ct);
        if (!m_func.EnsureZSafe(ct)) return false;

        base::Stopwatch sw;
        auto allUp = [&] {
            for (int k = 1; k <= NozzleCount; ++k) if (!Io().GetDi(IoMap::VacuumSensor(k))) return false;
            return true;
        };
        while (!allUp() && sw.ElapsedMilliseconds() < VacuumSensorWaitMs) base::Delay(10, ct);

        int missed = 0;
        for (int k = 1; k <= NozzleCount; ++k)
        {
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
        if (missed == NozzleCount)
            Log(std::format("Row {} of tray {}: no can picked — empty row?", r.Index + 1, r.Tray + 1), LogLevel::Warning);
        return true;
    }

    /// Soi từng nozzle đang giữ can: X/Z tới điểm CAM(k), chờ ổn định, Flow LocateCan. Vision tắt → bỏ qua, độ lệch 0.
    bool Routine_Auto::Vision(Row& r, std::stop_token ct)
    {
        if (!m_model->Auto.UseVision) return true;
        SetStep(EAutoState::S30_Vision, "CAM inspecting each nozzle");

        Io().SetDo(IoMap::DoCamLight, true);
        struct LightOff { IIoDevice& io; ~LightOff() { io.SetDo(IoMap::DoCamLight, false); } } lightOff{ Io() };
        bool first = true;
        for (int k = 1; k <= NozzleCount; ++k)
        {
            if (!r.Holding[k]) continue;
            NozzleSet(k, ECellState::Processing, "inspecting");
            auto p = CamAt(k);
            if (!m_func.EnsureZSafe(ct)) return false;
            if (!MoveAxes({ { AxisId::X, Coord(*p, AxisId::X) } }, ct)) return false;
            if (p->Has(AxisId::Z) && !MoveZTo(p->Get(AxisId::Z), ct)) return false;
            m_func.WaitSettle(ct);
            if (first) { base::Delay(m_sys()->LampSettleMs, ct); first = false; }

            std::string error;
            r.Seen[k] = Measure(k, r.RelX[k], r.RelZ[k], error, ct);
            if (!r.Seen[k])
            {
                r.Result[k] = (int)ECellState::Ng;
                CellSet(r.Tray, r.Index, k - 1, ECellState::Ng);
                NozzleSet(k, ECellState::Ng, "vision: " + error);
                NozzleDeviation(k, false, 0, 0);
                AlarmManager::RaiseEvent(AlarmCode::VisionNg, "Auto", std::format("nozzle {}: {}", k, error));
                Log(std::format("CAM measure failed, nozzle {}: {}", k, error), LogLevel::Warning);
            }
            else
            {
                const auto& nc = m_model->Calib.Nozzle(k);
                NozzleDeviation(k, true, r.RelX[k] - nc.RefX, r.RelZ[k] - nc.RefZ);
                NozzleSet(k, ECellState::Loaded, std::format("dX {:+.3f}, dZ {:+.3f}", r.RelX[k] - nc.RefX, r.RelZ[k] - nc.RefZ));
            }
        }
        return true;
    }

    /// Khắc từng nozzle còn OK: X/Z tới điểm Laser(k) + Trim + bù, DO kích, chờ MarkTimeMs. Độ lệch vượt ngưỡng → ô NG.
    bool Routine_Auto::Laser(Row& r, std::stop_token ct)
    {
        SetStep(EAutoState::S40_Laser, "Laser marking each nozzle");
        auto sys = m_sys();
        const bool useVision = m_model->Auto.UseVision;
        for (int k = 1; k <= NozzleCount; ++k)
        {
            if (!r.Holding[k] || r.Result[k] == (int)ECellState::Ng) continue;
            auto p = LaserAt(k);
            double baseX = Coord(*p, AxisId::X), baseZ = Coord(*p, AxisId::Z);
            auto dev = VmCycle::Deviation(m_model->Calib.Nozzle(k), useVision ? r.RelX[k] : 0.0, useVision ? r.RelZ[k] : 0.0,
                                          sys->LaserComp, baseX, baseZ, useVision && r.Seen[k]);
            if (!dev.Ok)
            {
                r.Result[k] = (int)ECellState::Ng;
                CellSet(r.Tray, r.Index, k - 1, ECellState::Ng);
                NozzleSet(k, ECellState::Ng, dev.Why);
                AlarmManager::RaiseEvent(AlarmCode::CompensationOutOfRange, "Auto", std::format("nozzle {}: {}", k, dev.Why));
                Log(std::format("Nozzle {} not marked: {}", k, dev.Why), LogLevel::Warning);
                continue;
            }

            NozzleSet(k, ECellState::Processing, std::format("marking at X {:.3f}, Z {:.3f}", dev.TargetX, dev.TargetZ));
            if (!m_func.EnsureZSafe(ct)) return false;
            if (!MoveAxes({ { AxisId::X, dev.TargetX } }, ct)) return false;
            if (!MoveZTo(dev.TargetZ, ct)) return false;
            m_func.WaitSettle(ct);
            Log(std::format("Nozzle {}: mark at X {:.3f}, Z {:.3f} (dX {:+.3f}, dZ {:+.3f})", k, dev.TargetX, dev.TargetZ, dev.Dx, dev.Dz));
            if (m_model->Auto.UseLaser) Mark(ct);
            r.Result[k] = (int)ECellState::Ok;
            NozzleSet(k, ECellState::Ok, "marked");
            m_cycleDone = m_cycleDone + 1;
        }
        return true;
    }

    /// Trả cả hàng về đúng ô: Y/X/Z tới điểm gắp, Z hạ, 7 van tắt, chờ, Z nâng.
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
        base::Delay(ap.VacuumOffMs, ct);
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
        int ok = 0, ng = 0;
        std::vector<int> result;
        for (int k = 1; k <= NozzleCount; ++k)
        {
            result.push_back(r.Result[k]);
            if (r.Result[k] == (int)ECellState::Ok) ++ok;
            else ++ng;
        }

        auto run = McDataManager::RunData();
        run->TrayRowsDone[r.Tray] = std::max(run->TrayRowsDone[r.Tray], r.Index + 1);
        run->LastRowResult = result;
        run->LastRowTray = r.Tray + 1;
        run->LastRowIndex = r.Index + 1;
        run->CountOk += ok;
        run->CountNg += ng;
        run->CountTotal += ok + ng;
        run->ConsecutiveNg = ng > 0 ? run->ConsecutiveNg + ng : 0;
        const double sec = m_cycleClock.Seconds();
        run->RecordCycle(sec, ok + ng);
        McDataManager::SaveRunningData();
        m_cycleClock.Reset();
        m_cycleDone = 0;

        m_lastRowText = std::format("Tray {} row {} done: {} OK, {} NG ({:.1f} s)", r.Tray + 1, r.Index + 1, ok, ng, sec);
        m_status.SetMessage(m_lastRowText);
        Log(m_lastRowText, ng > 0 ? LogLevel::Warning : LogLevel::Info);
    }

    // ───────────────────────── vision / laser ─────────────────────────

    bool Routine_Auto::Measure(int nozzle, double& relX, double& relZ, std::string& error, std::stop_token ct)
    {
        auto vision = m_vision ? m_vision() : nullptr;
        if (!vision) { error = "vision device not available"; return false; }
        auto sys = m_sys();
        const auto& names = sys->VisionOutputs;
        error = VmCycle::AxisVarProblem(&names);
        if (!error.empty()) return false;

        auto g = VmCycle::AxisGlobals(&m_motion, names, VmRunIds::Next(), nozzle);
        auto req = VmCycle::LocateRequest(names, g);
        auto r = vision->CallFlow(req, ct);
        auto read = VmCycle::ReadLocate(r, g, names);
        if (!read.CallOk) { error = read.CallError; return false; }
        if (!read.EchoOk) { error = read.EchoIssue; return false; }
        if (!read.Found) { error = "can not found"; return false; }
        if (!read.VmValid) { error = read.VmIssue; return false; }
        relX = read.RelX;
        relZ = read.RelZ;
        Log(std::format("LocateCan N{} RunId {}: axis X {:.3f}, Z {:.3f} → Obj ({:.3f}, {:.3f}), Rel ({:+.3f}, {:+.3f})",
                        nozzle, VmCycle::Fmt(VmCycle::Val(g, VmVars::RunId)), read.AxX, read.AxZ, read.X, read.Y, relX, relZ));
        return true;
    }

    void Routine_Auto::Mark(std::stop_token ct)
    {
        auto sys = m_sys();
        int hold = std::max(m_model->Auto.MarkTimeMs, sys->LaserComp.LaserPulseMinMs);
        IIoDevice& io = Io();
        io.SetDo(IoMap::DoLaserTrigger, true);
        struct Off { IIoDevice& io; ~Off() { io.SetDo(IoMap::DoLaserTrigger, false); } } off{ io };
        // Lệnh khắc không bị cắt ngang bởi STOP (bit RUN) — chỉ EMG/alarm (ct) mới cắt.
        base::Delay(hold, ct);
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
        int rows = model ? std::max(0, model->Auto.TrayRows) : 0;
        std::string s = std::format("Tray 1: {}/{} rows · Tray 2: {}/{} rows done", run->TrayRowsDone[0], rows, run->TrayRowsDone[1], rows);
        if (!run->LastRowResult.empty())
        {
            s += std::format("\nLast row (tray {} row {}):", run->LastRowTray, run->LastRowIndex);
            for (size_t i = 0; i < run->LastRowResult.size(); ++i)
                s += std::format(" N{}·{}", i + 1, run->LastRowResult[i] == (int)ECellState::Ok ? "OK" : "NG");
        }
        return s;
    }
}
