#pragma once
#include "Infor/AxisId.h"

#include <string>
#include <string_view>
#include <utility>

namespace scs
{
    /// <summary>
    /// Offset lỗi dùng chung cho mọi khối trục — PLAN.md §7.2.
    ///
    /// <para><b>Hai nhóm khác hẳn nhau:</b> Disconnected/DriverAlarm/HardLimit là ĐIỀU KIỆN tiếp
    /// diễn — vòng quét liên tục Raise lại lúc còn đúng và ClearCondition lúc hết, phát qua
    /// <c>AlarmManager::RaiseAxis</c>. Các mã còn lại là SỰ KIỆN một lần thử thất bại — phải phát
    /// qua <c>AlarmManager::RaiseAxisEvent</c> (tự ClearCondition ngay sau Raise) để RESET không bị
    /// kẹt vĩnh viễn (bug thật 2026-09-09: HomeTimeout không tự hết đã khoá SafeToMove cả máy).</para>
    /// </summary>
    enum class AxisFault
    {
        Disconnected = 0,        // +00 Mất kết nối (Fatal) — điều kiện tiếp diễn, RaiseAxis
        DriverAlarm = 1,         // +01 Alarm từ driver (Fatal) — điều kiện tiếp diễn, RaiseAxis
        MoveTimeout = 2,         // +02 Timeout chuyển động (Error) — sự kiện, RaiseAxisEvent
        SoftLimit = 3,           // +03 Chạm soft limit (Error) — sự kiện, RaiseAxisEvent
        HardLimit = 4,           // +04 Chạm hard limit POT/NOT (Warning) — điều kiện tiếp diễn, RaiseAxis
        HomeTimeout = 5,         // +05 Timeout về gốc / driver alarm lúc về gốc (Error) — sự kiện
        NotHomed = 6,            // +06 Chưa về gốc mà nhận lệnh chạy (Error) — sự kiện
        ServoOffUnexpected = 7,  // +07 Servo OFF ngoài ý muốn (Fatal) — sự kiện
        FollowingError = 8       // +08 Sai lệch vị trí quá lớn (Fatal) — chưa nơi nào Raise
    };

    /// <summary>
    /// Mã alarm. Chia khối theo nguồn phát, mỗi trục một dải 100 và dùng chung bảng offset
    /// <see cref="AxisFault"/>: 2205 = Z timeout về gốc. Máy Laser Marking có 3 trục: X 2000, Y 2100, Z 2200. Laser chỉ là
    /// một DO nên không có khối alarm laser.
    /// </summary>
    struct AlarmCode
    {
        // ── 1000–1099 Hệ thống ──
        static constexpr int ConfigInvalid = 1000;
        static constexpr int RoutineCrashed = 1001;
        static constexpr int ControlWatchdog = 1002;
        static constexpr int DiskFull = 1003;
        static constexpr int UndefinedAlarm = 1099;

        // ── 1100–1199 An toàn ── (1101, 1102, 1104 đã bỏ cùng cảm biến cửa — KHÔNG tái dùng)
        static constexpr int EmergencyStop = 1100;
        static constexpr int AirPressureLow = 1103;
        static constexpr int LightCurtainBroken = 1105;

        // ── 2000–2299 Trục (base + offset) — X, Y, Z ──
        static constexpr int AxisBaseX = 2000;
        static constexpr int AxisBaseY = 2100;
        static constexpr int AxisBaseZ = 2200;

        /// <summary>Mã trục nằm trong [AxisCodeMin, AxisCodeEnd).</summary>
        static constexpr int AxisCodeMin = 2000;
        static constexpr int AxisCodeEnd = 2300;

        // ── 3000–3099 I/O ──
        static constexpr int IoDisconnected = 3000;
        static constexpr int IoWriteFailed = 3001;
        static constexpr int IoModuleFault = 3002;            // R1EC báo lỗi module trong rack (0xF100:01 khác 0)

        // ── 3100–3199 Bus EtherCAT ──
        static constexpr int EtherCatBusUnavailable = 3100;   // không mở được bus
        static constexpr int EtherCatSlotInvalid = 3101;      // Connect All Axis: có slot không đạt

        // ── 4000–4099 Vision ──
        static constexpr int VisionDisconnected = 4000;
        static constexpr int VisionLoadFailed = 4001;
        static constexpr int VisionNotReady = 4002;
        static constexpr int VisionTriggerTimeout = 4003;
        static constexpr int VisionNotFound = 4004;
        static constexpr int VisionLowScore = 4005;
        static constexpr int VisionNotCalibrated = 4006;
        static constexpr int VisionContract = 4010;           // Flow trả số không khớp hợp đồng
        static constexpr int VisionCalibNotReady = 4011;      // hiệu chuẩn chưa sẵn sàng (cổng phía App)
        static constexpr int VisionCalibExpired = 4090;       // Warning

        // ── 5000–5099 Chu trình ──
        static constexpr int PickFailed = 5001;
        static constexpr int ProductDropped = 5002;
        static constexpr int PlaceFailed = 5003;
        static constexpr int CompensationOutOfRange = 5004;   // độ lệch vision vượt ngưỡng bù — Warning, ô NG
        static constexpr int PlaceTargetGuard = 5005;
        static constexpr int VisionNg = 5006;                 // vision không thấy can trên nozzle — Warning, ô NG
        static constexpr int ConsecutiveNg = 5010;
        static constexpr int TrayEmpty = 5020;                // Warning — thông báo chờ thay tray
        static constexpr int ResumeIntegrity = 5030;
        static constexpr int CannotReturnProduct = 5031;

        // ── 6000–6099 Dữ liệu ──
        static constexpr int ModelSaveFailed = 6000;
        static constexpr int ModelFileCorrupt = 6001;
        static constexpr int RunningDataFailed = 6002;

        /// <summary>Base của khối mã một trục; -1 nếu tên lạ.</summary>
        static int AxisBase(std::string_view axisName)
        {
            static constexpr std::pair<const char*, int> bases[] = { { "X", AxisBaseX }, { "Y", AxisBaseY }, { "Z", AxisBaseZ } };
            for (const auto& b : bases)
                if (axisName == b.first) return b.second;
            return -1;
        }

        /// <summary>Mã alarm của một trục: base của trục cộng offset lỗi. Tên lạ → UndefinedAlarm.</summary>
        static int ForAxis(std::string_view axisName, AxisFault fault)
        {
            int base = AxisBase(axisName);
            return base < 0 ? UndefinedAlarm : base + (int)fault;
        }

        /// <summary>Tách ngược mã trục về (tên trục, loại lỗi). Trả về false nếu không phải mã trục.</summary>
        static bool TryParseAxis(int code, std::string& axisName, AxisFault& fault)
        {
            axisName.clear();
            fault = AxisFault::Disconnected;
            if (code < AxisCodeMin || code >= AxisCodeEnd) return false;

            int b = code / 100 * 100;
            for (const char* name : AxisId::All)
            {
                if (AxisBase(name) != b) continue;
                axisName = name;
                fault = (AxisFault)(code - b);
                return true;
            }
            return false;
        }
    };
}
