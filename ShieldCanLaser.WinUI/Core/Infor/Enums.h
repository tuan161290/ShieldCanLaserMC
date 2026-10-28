#pragma once
// Giá trị số của các enum được lưu trong JSON/log — KHÔNG đổi thứ tự.

namespace scs
{
    /// <summary>Trạng thái máy — PLAN.md §3.3.</summary>
    enum class EMcState
    {
        Init = 0,       // Nạp config, kết nối thiết bị. Chưa cho chuyển động.
        NotHomed,       // Đã kết nối, chưa về gốc. Chỉ jog tay tốc độ giới hạn.
        Homing,         // Đang chạy chuỗi Origin.
        Idle,           // Đã về gốc, đứng yên, sẵn sàng.
        Armed,          // Đã chọn Auto và đủ điều kiện — chờ nút START vật lý (chưa có chu trình nào chạy).
        Running,        // Đang chạy Auto.
        Pausing,        // Đã nhận STOP, đang chạy nốt tới checkpoint an toàn.
        Paused,         // Dừng ở checkpoint, giữ tiến độ và sản phẩm trên tay hút.
        Held,           // STOP lúc đang chạy: mọi trục dừng ngay, Start chạy tiếp đúng lệnh dở.
        WaitOperator,   // Chờ người thao tác — đổi tray, xác nhận.
        Alarm,          // Có alarm mức Error/Fatal đang active.
        Emg             // Mạch an toàn hở.
    };

    /// <summary>Chế độ vận hành — chọn bằng nút AUTO/MANUAL trên trang Main.</summary>
    enum class EMcMode { Manual = 0, Auto };

    /// <summary>Bước của chuỗi Auto — máy Laser Marking (PLAN-LASER.md §2), xem Routine_Auto.</summary>
    enum class EAutoState
    {
        S00_Ready = 0,
        S10_WaitStart = 10,      // chờ START (đã có tray)
        S20_PickRow = 20,        // gắp một hàng 7 Shield CAN
        S30_Vision = 30,         // CAM soi từng nozzle, đo độ lệch
        S40_Laser = 40,          // khắc từng nozzle, bù X/Z + góc
        S50_PlaceRow = 50,       // trả hàng về đúng ô tray
        S60_WaitTray = 60        // hết 2 tray — chờ thay tray rồi bấm START
    };

    /// <summary>Bước của chuỗi Origin — xem Routine_Origin. Giữ số của máy Slit (O4 không còn dùng).</summary>
    enum class EOriginState
    {
        O0_PreCheck = 0, O1_ServoOn = 1, O2_HomeZ = 2, O3_GateZSafe = 3,
        O5_HomeXY = 5, O6_Standby = 6, O7_Finish = 7, Aborted = 99
    };

    enum class EJudge { None = 0, Ok, Ng };

    /// <summary>Mức alarm — PLAN.md §7.1.</summary>
    enum class EAlarmLevel
    {
        Info = 0,   // Chỉ ghi log, không hiện banner.
        Warning,    // Không dừng máy, đèn vàng nháy.
        Error,      // Dừng chu trình ngay, giữ servo. Cần RESET + hết nguyên nhân.
        Fatal       // Cắt servo, bắt buộc Origin lại. Cần RESET + hết nguyên nhân.
    };

    /// <summary>Trạng thái một ô tray.</summary>
    enum class ECellState { Empty = 0, Loaded, Ok, Ng, Processing, Skipped };

    /// <summary>Thứ tự quét ô trong tray.</summary>
    enum class EScanOrder { RowMajor = 0, ColMajor, Serpentine };

    /// <summary>Màu đèn tháp — PLAN.md §3.6.</summary>
    enum class ETowerLamp { Off = 0, Green, GreenBlink, Yellow, YellowBlink, Red, RedBlink };

    inline const char* ToString(EMcState v)
    {
        static const char* n[] = { "Init", "NotHomed", "Homing", "Idle", "Armed", "Running", "Pausing", "Paused", "Held", "WaitOperator", "Alarm", "Emg" };
        return (int)v >= 0 && (int)v < 12 ? n[(int)v] : "?";
    }
    inline const char* ToString(EMcMode v) { return v == EMcMode::Manual ? "Manual" : v == EMcMode::Auto ? "Auto" : "?"; }
    inline const char* ToString(EAutoState v)
    {
        switch (v)
        {
            case EAutoState::S00_Ready:     return "S00_Ready";
            case EAutoState::S10_WaitStart: return "S10_WaitStart";
            case EAutoState::S20_PickRow:   return "S20_PickRow";
            case EAutoState::S30_Vision:    return "S30_Vision";
            case EAutoState::S40_Laser:     return "S40_Laser";
            case EAutoState::S50_PlaceRow:  return "S50_PlaceRow";
            case EAutoState::S60_WaitTray:  return "S60_WaitTray";
        }
        return "?";
    }
    inline const char* ToString(EOriginState v)
    {
        switch (v)
        {
            case EOriginState::O0_PreCheck:  return "O0_PreCheck";
            case EOriginState::O1_ServoOn:   return "O1_ServoOn";
            case EOriginState::O2_HomeZ:     return "O2_HomeZ";
            case EOriginState::O3_GateZSafe: return "O3_GateZSafe";
            case EOriginState::O5_HomeXY:    return "O5_HomeXY";
            case EOriginState::O6_Standby:   return "O6_Standby";
            case EOriginState::O7_Finish:    return "O7_Finish";
            case EOriginState::Aborted:      return "Aborted";
        }
        return "?";
    }
    inline const char* ToString(EJudge v)
    {
        static const char* n[] = { "None", "Ok", "Ng" };
        return (int)v >= 0 && (int)v < 3 ? n[(int)v] : "?";
    }
    inline const char* ToString(EAlarmLevel v)
    {
        static const char* n[] = { "Info", "Warning", "Error", "Fatal" };
        return (int)v >= 0 && (int)v < 4 ? n[(int)v] : "?";
    }
    inline const char* ToString(ECellState v)
    {
        static const char* n[] = { "Empty", "Loaded", "Ok", "Ng", "Processing", "Skipped" };
        return (int)v >= 0 && (int)v < 6 ? n[(int)v] : "?";
    }
    inline const char* ToString(EScanOrder v)
    {
        static const char* n[] = { "RowMajor", "ColMajor", "Serpentine" };
        return (int)v >= 0 && (int)v < 3 ? n[(int)v] : "?";
    }
    inline const char* ToString(ETowerLamp v)
    {
        static const char* n[] = { "Off", "Green", "GreenBlink", "Yellow", "YellowBlink", "Red", "RedBlink" };
        return (int)v >= 0 && (int)v < 7 ? n[(int)v] : "?";
    }
}
