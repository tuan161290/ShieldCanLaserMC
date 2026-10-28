#pragma once
#include <format>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>
    /// Bản đồ I/O trên Remote IO EtherCAT — bộ ghép Leadshine R1EC, cắm theo thứ tự: module 32 ngõ vào rồi module 32
    /// ngõ ra (như máy Slit Assy). Ngõ vào đánh số <b>X000…X031</b>, ngõ ra <b>Y000…Y031</b> (thập phân, = chỉ số bit
    /// ở đây). Bit DI/DO nối liên tục theo thứ tự module trong rack (EtherCatIoDevice).
    ///
    /// <para><b>BẢN TẠM</b> (PLAN-LASER.md §5): máy Laser Marking chưa có sơ đồ điện — số chân xếp theo thứ tự hợp lý,
    /// chốt lại theo bản vẽ. Mọi nơi khác chỉ tham chiếu qua hằng số ở đây, không viết số bit trực tiếp.</para>
    ///
    /// <para><b>Quy ước NC.</b> Tín hiệu an toàn (EmgOk, LightCurtainOk, AirOk) đấu thường đóng / OSSD: <b>1 = bình
    /// thường</b>, <b>0 = đã tác động hoặc đứt dây</b>.</para>
    /// </summary>
    struct IoMap
    {
        /// <summary>Số nozzle (đầu hút) N1…N7 trên cụm X–Z — mỗi nozzle một van chân không và một cảm biến chân không.
        /// Cũng là số cột của tray: gắp cả hàng một lượt.</summary>
        static constexpr int NozzleCount = 7;

        /// <summary>Số tray Shield CAN trên trục Y — mỗi tray TrayRows hàng × 7 cột.</summary>
        static constexpr int TrayCount = 2;

        // ── DI ──
        static constexpr int BtnStart = 0;              // X000 — bắt sườn lên. Lúc Armed: chạy; tạm dừng: chạy tiếp; chờ tray: tray đã thay.
        static constexpr int BtnStop = 1;               // X001 — bắt sườn lên.
        static constexpr int EmgOk = 2;                 // X002 — NC: 1 = mạch an toàn đóng.
        static constexpr int BtnReset = 3;              // X003 — cùng tác dụng nút RESET trên màn hình.
        static constexpr int LightCurtainOk = 4;        // X004 — OSSD rèm sáng: 1 = thông. Chỉ đọc khi SystemData.HasLightCurtain.
        static constexpr int TrayPresentFirst = 5;      // X005, X006 — cảm biến có tray 1, 2. Chỉ đọc khi SystemData.HasTraySensors.
        static constexpr int VacuumSensorFirst = 7;     // X007…X013 — cảm biến chân không N1…N7.
        static constexpr int AirOk = 14;                // X014 — NC: 1 = đủ áp suất khí. Chỉ đọc khi SystemData.HasAirSensor.

        /// <summary>DI cảm biến tray 1…2 (X005, X006).</summary>
        static constexpr int TrayPresent(int tray) { return TrayPresentFirst + tray - 1; }

        /// <summary>DI cảm biến chân không của nozzle 1…7 (X007…X013).</summary>
        static constexpr int VacuumSensor(int nozzle) { return VacuumSensorFirst + nozzle - 1; }

        // ── DO ──
        static constexpr int DoLampStartBtn = 0;        // Y000 — đèn nút START.
        static constexpr int DoLampStopBtn = 1;         // Y001 — đèn nút STOP.
        static constexpr int DoLampResetBtn = 2;        // Y002 — đèn nút RESET (sáng khi còn alarm).
        static constexpr int DoVacuumFirst = 3;         // Y003…Y009 — van chân không N1…N7.
        static constexpr int DoLampRed = 10;            // Y010 — đèn tháp đỏ.
        static constexpr int DoLampYellow = 11;         // Y011 — đèn tháp vàng.
        static constexpr int DoLampGreen = 12;          // Y012 — đèn tháp xanh.
        static constexpr int DoBuzzer = 13;             // Y013 — còi.
        static constexpr int DoCamLight = 14;           // Y014 — đèn chiếu sáng camera.
        static constexpr int DoLaserTrigger = 15;       // Y015 — kích khắc laser: ON, giữ MarkTimeMs (model), OFF. Laser không có
                                                        //        giao tiếp hay tín hiệu "khắc xong" (Tuấn 2026-10-27).

        /// <summary>DO van chân không của nozzle 1…7 (Y003…Y009).</summary>
        static constexpr int DoVacuum(int nozzle) { return DoVacuumFirst + nozzle - 1; }

        static constexpr int DiCount = 32;         // R1EC slot 1: module 32 DI → X000…X031.
        static constexpr int DoCount = 32;         // R1EC slot 2: module 32 DO → Y000…Y031.

        /// <summary>Tên kênh theo bản vẽ điện: DI bit 0 → "X000", DO bit 31 → "Y031".</summary>
        static std::string DiLabel(int bit) { return std::format("X{:03}", bit); }
        static std::string DoLabel(int bit) { return std::format("Y{:03}", bit); }

        /// <summary>Nhãn hiển thị trên bảng đèn DI/DO của trang Manual.</summary>
        static const std::vector<std::string>& DiNames()
        {
            static const std::vector<std::string> names = [] {
                std::vector<std::string> n(DiCount, "(spare)");
                n[BtnStart] = "START button";
                n[BtnStop] = "STOP button";
                n[EmgOk] = "EMG OK (NC)";
                n[BtnReset] = "RESET button";
                n[LightCurtainOk] = "Photo safety sensor OK";
                for (int tray = 1; tray <= TrayCount; ++tray)
                    n[TrayPresent(tray)] = std::format("Tray {} sensor", tray);
                for (int k = 1; k <= NozzleCount; ++k)
                    n[VacuumSensor(k)] = std::format("Vacuum sensor N{}", k);
                n[AirOk] = "Air pressure OK (NC)";
                return n;
            }();
            return names;
        }

        static const std::vector<std::string>& DoNames()
        {
            static const std::vector<std::string> names = [] {
                std::vector<std::string> n(DoCount, "(spare)");
                n[DoLampStartBtn] = "START button lamp";
                n[DoLampStopBtn] = "STOP button lamp";
                n[DoLampResetBtn] = "RESET button lamp";
                for (int k = 1; k <= NozzleCount; ++k)
                    n[DoVacuum(k)] = std::format("Solenoid vacuum N{}", k);
                n[DoLampRed] = "Tower lamp red";
                n[DoLampYellow] = "Tower lamp yellow";
                n[DoLampGreen] = "Tower lamp green";
                n[DoBuzzer] = "Buzzer";
                n[DoCamLight] = "CAM light";
                n[DoLaserTrigger] = "Laser trigger";
                return n;
            }();
            return names;
        }
    };
}
