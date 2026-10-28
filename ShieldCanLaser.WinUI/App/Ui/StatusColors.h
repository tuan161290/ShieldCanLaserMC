#pragma once
// Bảng màu trạng thái — thay các IValueConverter của bản WPF (Converters\StatusConverters.cs). Trả ARGB;
// dùng ui::Solid(argb) để ra brush.
#include <cstdint>

#include "Infor/Enums.h"

namespace ui
{
    /// <summary>Mức alarm → màu nền nhãn/dòng.</summary>
    inline uint32_t AlarmLevelColor(scs::EAlarmLevel level)
    {
        switch (level)
        {
        case scs::EAlarmLevel::Fatal:   return 0xFFC42B1C;
        case scs::EAlarmLevel::Error:   return 0xFFE94949;
        case scs::EAlarmLevel::Warning: return 0xFF9D5D00;
        default:                        return 0xFF606060;
        }
    }

    /// <summary>
    /// Đèn tháp → màu đặc cho nhãn trạng thái ở thanh tiêu đề. CỐ Ý lấy thẳng từ Status.TowerLamp (nguồn
    /// sự thật, do McControl tính) thay vì tự suy lại từ EMcState lần hai — tránh nhãn trên màn hình và
    /// đèn tháp vật lý nói hai màu khác nhau cho cùng một trạng thái máy. Blink chỉ là hiệu ứng nháy trên
    /// đèn vật lý; nhãn tĩnh nên gộp về màu gốc.
    /// </summary>
    inline uint32_t TowerLampColor(scs::ETowerLamp lamp)
    {
        switch (lamp)
        {
        case scs::ETowerLamp::Green:
        case scs::ETowerLamp::GreenBlink:  return 0xFF389853;
        case scs::ETowerLamp::Red:
        case scs::ETowerLamp::RedBlink:    return 0xFFC42B1C;
        case scs::ETowerLamp::Yellow:
        case scs::ETowerLamp::YellowBlink: return 0xFF9D5D00;
        default:                           return 0xFF606060;
        }
    }

    /// <summary>Cùng tông với TowerLampColor nhưng chỉ là lớp phủ mờ — nền của nhãn kiểu Windows 11.</summary>
    inline uint32_t TowerLampTintColor(scs::ETowerLamp lamp)
    {
        return (TowerLampColor(lamp) & 0x00FFFFFF) | 0x1C000000;
    }

    /// <summary>Một bóng của đèn tháp vẽ trên màn hình: which = 'R' / 'Y' / 'G'.</summary>
    inline uint32_t TowerBulbColor(scs::ETowerLamp lamp, char which)
    {
        const uint32_t off = 0xFF3A3A3A;
        switch (which)
        {
        case 'R': return (lamp == scs::ETowerLamp::Red || lamp == scs::ETowerLamp::RedBlink) ? 0xFFE94949 : off;
        case 'Y': return (lamp == scs::ETowerLamp::Yellow || lamp == scs::ETowerLamp::YellowBlink) ? 0xFFFFC800 : off;
        case 'G': return (lamp == scs::ETowerLamp::Green || lamp == scs::ETowerLamp::GreenBlink) ? 0xFF389853 : off;
        default:  return off;
        }
    }

    /// <summary>Trạng thái máy → màu banner trạng thái ở trang Main.</summary>
    inline uint32_t McStateColor(scs::EMcState state)
    {
        switch (state)
        {
        case scs::EMcState::Running:      return 0xFF389853;
        case scs::EMcState::Idle:         return 0xFF0F7B0F;
        case scs::EMcState::Armed:        return 0xFF9D5D00;
        case scs::EMcState::Paused:
        case scs::EMcState::Held:
        case scs::EMcState::Pausing:
        case scs::EMcState::WaitOperator: return 0xFF9D5D00;
        case scs::EMcState::Alarm:        return 0xFFC42B1C;
        case scs::EMcState::Emg:          return 0xFF8B0000;
        case scs::EMcState::Homing:       return 0xFF0078D4;
        default:                          return 0xFF606060;
        }
    }
}
