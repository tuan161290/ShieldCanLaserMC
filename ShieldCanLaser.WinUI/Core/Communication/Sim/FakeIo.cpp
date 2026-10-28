#include "Communication/Sim/FakeIo.h"

namespace scs
{
    namespace
    {
        using Lock = std::lock_guard<std::recursive_mutex>;

        /// <summary>Các DI là nút nhấn-nhả — mô phỏng tự nhả sau một vòng quét.</summary>
        constexpr int MomentaryButtons[] = { IoMap::BtnStart, IoMap::BtnStop, IoMap::BtnReset };
    }

    FakeIo::FakeIo()
    {
        // Tín hiệu an toàn đấu NC: 1 = bình thường. Hai tray có mặt.
        m_di[IoMap::EmgOk] = true;
        m_di[IoMap::LightCurtainOk] = true;
        m_di[IoMap::AirOk] = true;
        for (int t = 1; t <= IoMap::TrayCount; ++t) m_di[IoMap::TrayPresent(t)] = true;
    }

    bool FakeIo::GetDi(int bit)
    {
        Lock l(m_lock);
        return bit >= 0 && bit < IoMap::DiCount && m_di[bit];
    }

    bool FakeIo::GetDo(int bit)
    {
        Lock l(m_lock);
        return bit >= 0 && bit < IoMap::DoCount && m_do[bit];
    }

    void FakeIo::SetDo(int bit, bool value)
    {
        if (bit < 0 || bit >= IoMap::DoCount) return;
        {
            Lock l(m_lock);
            if (m_do[bit] == value) return;
            m_do[bit] = value;
            // Cảm biến chân không của nozzle bám theo van hút của nó (trừ nozzle đặt hút trượt).
            for (int k = 1; k <= IoMap::NozzleCount; ++k)
                if (bit == IoMap::DoVacuum(k)) m_di[IoMap::VacuumSensor(k)] = value && !m_pickFail[k];
            if (bit == IoMap::DoLaserTrigger && value) ++m_laserPulses;
        }
        if (AfterSetDo) AfterSetDo(bit, value);
    }

    void FakeIo::Refresh()
    {
        Lock l(m_lock);
        m_diPrev = m_di;

        // Nút bấm giả lập: giữ đúng MỘT vòng quét rồi tự nhả, để bên tiêu thụ buộc phải
        // xử lý bằng sườn lên chứ không phải bằng mức.
        for (int bit : m_pendingPress)
            if (bit >= 0 && bit < IoMap::DiCount) m_di[bit] = true;

        for (int bit : m_pendingPress)
            if (bit >= 0 && bit < IoMap::DiCount) m_diPrev[bit] = false;

        m_pendingPress.clear();

        for (int i : MomentaryButtons)
        {
            // Nút không được giữ: nếu vòng trước đã ở mức 1 thì vòng này nhả.
            if (m_diPrev[i]) m_di[i] = false;
        }
    }

    bool FakeIo::RisingEdge(int bit)
    {
        Lock l(m_lock);
        return bit >= 0 && bit < IoMap::DiCount && m_di[bit] && !m_diPrev[bit];
    }

    bool FakeIo::FallingEdge(int bit)
    {
        Lock l(m_lock);
        return bit >= 0 && bit < IoMap::DiCount && !m_di[bit] && m_diPrev[bit];
    }

    void FakeIo::SimPress(int diBit)
    {
        Lock l(m_lock);
        m_pendingPress.insert(diBit);
    }

    void FakeIo::SimToggle(int diBit)
    {
        Lock l(m_lock);
        if (diBit < 0 || diBit >= IoMap::DiCount) return;
        m_di[diBit] = !m_di[diBit];
    }

    void FakeIo::SimSetPickFail(int nozzle, bool fail)
    {
        Lock l(m_lock);
        if (nozzle < 1 || nozzle > IoMap::NozzleCount) return;
        m_pickFail[nozzle] = fail;
        m_di[IoMap::VacuumSensor(nozzle)] = m_do[IoMap::DoVacuum(nozzle)] && !fail;
    }

    void FakeIo::SimSet(int diBit, bool value)
    {
        Lock l(m_lock);
        if (diBit < 0 || diBit >= IoMap::DiCount) return;
        m_di[diBit] = value;
    }
}
