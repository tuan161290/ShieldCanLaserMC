#include "Controller/SafetyMonitor.h"

#include "Infor/AlarmManager.h"
#include "Infor/IoMap.h"
#include "Utility/Log/LogWorker.h"

namespace scs
{
    SafetyMonitor::SafetyMonitor(MotionConnect& motion, McStatus& status, std::function<std::shared_ptr<SystemData>()> sys)
        : m_motion(motion), m_status(status), m_sys(std::move(sys))
    {
    }

    std::stop_token SafetyMonitor::AbortToken() const
    {
        std::lock_guard<std::mutex> lock(m_ctsLock);
        return m_cts.get_token();
    }

    std::string SafetyMonitor::BlockReason() const
    {
        std::lock_guard<std::mutex> lock(m_reasonMutex);
        return m_blockReason;
    }

    void SafetyMonitor::SetBlockReason(const char* reason)
    {
        std::lock_guard<std::mutex> lock(m_reasonMutex);
        m_blockReason = reason;
    }

    void SafetyMonitor::RenewToken()
    {
        std::lock_guard<std::mutex> lock(m_ctsLock);
        if (!m_cts.stop_requested()) return;
        m_cts = std::stop_source();
    }

    void SafetyMonitor::Abort(const std::string& reason)
    {
        std::stop_source cts;
        {
            std::lock_guard<std::mutex> lock(m_ctsLock);
            if (m_cts.stop_requested()) return;
            cts = m_cts;
        }
        // Huỷ NGOÀI khoá: các callback của token (lệnh trục đang chờ) chạy ngay trên luồng này.
        if (!cts.request_stop()) return;
        LogWorker::Instance().RecordLog("All motion aborted: " + reason, LogLevel::Error, "Safety");
        m_motion.StopAll();
    }

    void SafetyMonitor::Evaluate()
    {
        auto sys = m_sys();
        IIoDevice* io = m_io.get();

        bool emgOk = io != nullptr && io->IsConnected() && io->GetDi(IoMap::EmgOk);
        bool ioOk = io != nullptr && io->IsConnected();
        bool curtainOk = !sys->HasLightCurtain || (io != nullptr && io->GetDi(IoMap::LightCurtainOk));
        bool airOk = !sys->HasAirSensor || (io != nullptr && io->GetDi(IoMap::AirOk));

        m_status.SetEmgOk(emgOk);
        m_status.SetLightCurtainOk(curtainOk);
        m_status.SetAirOk(airOk);
        m_status.SetIoConnected(ioOk);
        m_status.SetMotionConnected(m_motion.AllConnected());

        // ── Remote IO ──
        //
        // EMG và rèm sáng đều đọc qua Remote IO EtherCAT. Hệ quả trực tiếp: MẤT KẾT NỐI REMOTE IO LÀ
        // MÙ HOÀN TOÀN — không còn đường nào biết EMG có đang bị nhấn hay rèm có bị cắt hay không. Nên
        // mất kết nối phải được xử lý như chính sự cố tệ nhất mà nó che mất: dừng ngay mọi chuyển
        // động, không chỉ chặn lệnh mới.
        if (!ioOk)
        {
            AlarmManager::Raise(AlarmCode::IoDisconnected);
            Abort("I/O card disconnected — cannot read EMG and photo safety");
        }
        else
        {
            AlarmManager::ClearCondition(AlarmCode::IoDisconnected);
        }

        // ── EMG (NC: 0 = đã nhấn hoặc đứt dây) ──
        if (ioOk && !emgOk)
        {
            AlarmManager::Raise(AlarmCode::EmergencyStop);
            Abort("EMG");
        }
        else
        {
            AlarmManager::ClearCondition(AlarmCode::EmergencyStop);
        }

        // ── Rèm sáng ──
        // Rèm bị cắt thì DỪNG NGAY mọi trục dù đang ở chế độ nào, vì người có thể thò tay qua rèm bất cứ lúc nào — kể
        // cả lúc thợ đang jog tay ở Manual — và chặn lệnh mới tới khi rèm thông. Chu trình Auto thì tạm dừng như STOP,
        // rèm thông rồi bấm START chạy tiếp đúng chỗ dở — không alarm, không RESET. Rèm thông trở lại cũng không tự
        // chạy: phải có người bấm START.
        if (ioOk && !curtainOk)
        {
            if (m_curtainWasOk)
            {
                m_curtainWasOk = false;
                LogWorker::Instance().RecordLog("Photo safety triggered — all motion stopped", LogLevel::Warning, "Safety");
                if (m_curtainBroken) m_curtainBroken();
                m_motion.StopAll();
            }
        }
        else if (ioOk)
        {
            m_curtainWasOk = true;
        }

        // ── Áp suất khí (chỉ khi máy có cảm biến — SystemData.HasAirSensor) ──
        if (ioOk && !airOk) AlarmManager::Raise(AlarmCode::AirPressureLow);
        else AlarmManager::ClearCondition(AlarmCode::AirPressureLow);

        // ── Trục có alarm thì dừng luôn, không chờ routine phát hiện ──
        if (m_motion.AnyAlarm() || !m_motion.AllConnected())
        {
            EMcState st = m_status.State();
            if (st == EMcState::Running || st == EMcState::Homing)
                Abort("axis error or disconnected");
        }

        // ── Kết luận ──
        m_status.SetSafetyChainOk(emgOk && curtainOk && airOk);
        m_safeToMove = ComputeSafeToMove(ioOk, emgOk, curtainOk, airOk);
    }

    bool SafetyMonitor::ComputeSafeToMove(bool ioOk, bool emgOk, bool curtainOk, bool airOk)
    {
        if (!ioOk)                     { SetBlockReason("I/O card disconnected"); return false; }
        if (!emgOk)                    { SetBlockReason("EMG active"); return false; }
        if (!curtainOk)                { SetBlockReason("photo safety interrupted"); return false; }
        if (AlarmManager::HasError())  { SetBlockReason("alarm not cleared"); return false; }
        if (!m_motion.AllConnected())  { SetBlockReason("axis disconnected"); return false; }
        if (m_motion.AnyAlarm())       { SetBlockReason("axis alarm"); return false; }
        if (!airOk)                    { SetBlockReason("air pressure low"); return false; }

        SetBlockReason("");
        return true;
    }
}
