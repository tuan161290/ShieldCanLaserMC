#pragma once
#include <atomic>
#include <mutex>
#include <string>

#include "Infor/Enums.h"

namespace scs
{
    /// <summary>
    /// Trạng thái máy hiện thời — UI đọc theo chu kỳ (poll), không có thông báo đổi giá trị. Chỉ McControl,
    /// SafetyMonitor và các routine được ghi; mọi nơi khác chỉ đọc. Mọi getter an toàn đa luồng: số là
    /// atomic, chuỗi trả BẢN SAO dưới khoá.
    /// </summary>
    class McStatus
    {
    public:
        McStatus() = default;
        McStatus(const McStatus&) = delete;
        McStatus& operator=(const McStatus&) = delete;

#define SCS_STATUS_PROP(T, Name, init) \
        T Name() const { return m_##Name.load(); } \
        void Set##Name(T v) { m_##Name.store(v); } \
    private: std::atomic<T> m_##Name{ init }; public:

        SCS_STATUS_PROP(EMcState, State, EMcState::Init)
        SCS_STATUS_PROP(EMcMode, Mode, EMcMode::Manual)
        SCS_STATUS_PROP(EOriginState, OriginStep, EOriginState::O0_PreCheck)
        SCS_STATUS_PROP(bool, OrgOk, false)
        SCS_STATUS_PROP(EAutoState, AutoStep, EAutoState::S00_Ready)

        // ── An toàn ──
        SCS_STATUS_PROP(bool, EmgOk, false)
        SCS_STATUS_PROP(bool, LightCurtainOk, true)
        SCS_STATUS_PROP(bool, AirOk, true)
        SCS_STATUS_PROP(bool, SafetyChainOk, false)

        // ── Kết nối ──
        SCS_STATUS_PROP(bool, MotionConnected, false)
        SCS_STATUS_PROP(bool, IoConnected, false)
        SCS_STATUS_PROP(bool, VisionConnected, false)
        SCS_STATUS_PROP(bool, Simulation, false)

        // ── Hiển thị ──
        SCS_STATUS_PROP(ETowerLamp, TowerLamp, ETowerLamp::Off)
        SCS_STATUS_PROP(bool, Buzzer, false)
        SCS_STATUS_PROP(bool, BuzzerMuted, false)
#undef SCS_STATUS_PROP

        std::string ModelName() const { std::lock_guard<std::mutex> l(m_textMutex); return m_modelName; }
        void SetModelName(std::string v) { std::lock_guard<std::mutex> l(m_textMutex); m_modelName = std::move(v); }

        /// <summary>Dòng nhắc hiện dưới nút bấm — nói rõ vì sao lệnh vừa rồi không thực hiện được.</summary>
        std::string Message() const { std::lock_guard<std::mutex> l(m_textMutex); return m_message; }
        void SetMessage(std::string v) { std::lock_guard<std::mutex> l(m_textMutex); m_message = std::move(v); }

        std::string AutoStepText() const;
        std::string StateText() const;

    private:
        mutable std::mutex m_textMutex;
        std::string m_modelName = "(not loaded)";
        std::string m_message;
    };
}
