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
    /// <para><b>Danh sách IO</b> theo Tuấn 2026-11-03 — đối chiếu lại với bản vẽ điện. Mọi nơi khác chỉ tham chiếu qua
    /// hằng số ở đây, không viết số bit trực tiếp. Máy KHÔNG có nút RESET, nút tắt còi hay cảm biến áp suất khí — RESET
    /// và tắt còi bấm trên màn hình.</para>
    ///
    /// <para><b>Quy ước NC.</b> Tín hiệu an toàn (EmgOk, LightCurtainOk) đấu thường đóng / OSSD: <b>1 = bình thường</b>,
    /// <b>0 = đã tác động hoặc đứt dây</b>.</para>
    /// </summary>
    struct IoMap
    {
        /// <summary>Số nozzle (đầu hút) N1…N7 trên cụm X–Z — mỗi nozzle một van chân không, một van thổi và một cảm biến
        /// chân không. Cũng là số cột của tray: gắp cả hàng một lượt.</summary>
        static constexpr int NozzleCount = 7;

        /// <summary>Số kênh chân không đã đi dây: N1…N7 và kênh 8 dự phòng (X018, Y023, Y031).</summary>
        static constexpr int VacuumChannelCount = 8;

        /// <summary>Số tray Shield CAN trên trục Y — mỗi tray TrayRows hàng × 7 cột.</summary>
        static constexpr int TrayCount = 2;

        // ── DI ──
        static constexpr int BtnStart = 0;              // X000 — bắt sườn lên. Lúc Armed: chạy; tạm dừng: chạy tiếp; chờ tray: tray đã thay.
        static constexpr int BtnStop = 1;               // X001 — bắt sườn lên.
        static constexpr int EmgOk = 2;                 // X002 — NC: 1 = mạch an toàn đóng.
        static constexpr int BtnStart2 = 3;             // X003 — nút START thứ hai, cùng tác dụng BtnStart.
        static constexpr int DiLaserReady = 4;          // X004 — laser sẵn sàng. Chu trình chưa đọc, chỉ hiện ở trang Manual.
        static constexpr int DiLaserBusy = 5;           // X005 — laser đang khắc. Chu trình chưa đọc.
        static constexpr int DiLaserEnd = 6;            // X006 — laser khắc xong. Chu trình chưa đọc.
        static constexpr int TrayPresentFirst = 8;      // X008, X009 — cảm biến có tray 1, 2. Chỉ đọc khi SystemData.HasTraySensors.
        static constexpr int LightCurtainOk = 10;       // X010 — OSSD rèm sáng: 1 = thông. Chỉ đọc khi SystemData.HasLightCurtain.
        static constexpr int VacuumSensorFirst = 11;    // X011…X017 — cảm biến chân không N1…N7; X018 — kênh 8 dự phòng.

        /// <summary>DI cảm biến tray 1…2 (X008, X009).</summary>
        static constexpr int TrayPresent(int tray) { return TrayPresentFirst + tray - 1; }

        /// <summary>DI cảm biến chân không của kênh 1…8 (X011…X018).</summary>
        static constexpr int VacuumSensor(int nozzle) { return VacuumSensorFirst + nozzle - 1; }

        // ── DO ──
        static constexpr int DoLampStartBtn = 0;        // Y000 — đèn nút START.
        static constexpr int DoLampStopBtn = 1;         // Y001 — đèn nút STOP.
        static constexpr int DoLampStartBtn2 = 2;       // Y002 — đèn nút START thứ hai, sáng cùng Y000.
        static constexpr int LaserStart = 3;            // Y003 — kích khắc laser: ON, giữ MarkTimeMs (model), OFF.
        static constexpr int LaserStop = 4;             // Y004 — dừng laser: một xung khi lệnh khắc bị EMG/alarm cắt ngang.
        static constexpr int DoVacuumFirst = 6;         // Y006…Y010 — van chân không N1…N5.
        static constexpr int DoTowerLampRed = 12;       // Y012 — đèn tháp đỏ.
        static constexpr int DoTowerLampYellow = 13;    // Y013 — đèn tháp vàng.
        static constexpr int DoTowerLampGreen = 14;     // Y014 — đèn tháp xanh.
        static constexpr int DoBuzzer = 15;             // Y015 — còi.
        static constexpr int DoLightCam = 18;           // Y018 — đèn chiếu sáng camera.
        static constexpr int ZAxisBrakeRelease = 20;    // Y020 — nhả phanh trục Z. Vòng quét giữ ON khi và chỉ khi servo Z đang bật.
        static constexpr int DoVacuumSixth = 21;        // Y021…Y023 — van chân không N6, N7, kênh 8 dự phòng.
        static constexpr int VacuumBlowerFirst = 24;    // Y024…Y030 — van thổi N1…N7; Y031 — kênh 8 dự phòng.

        /// <summary>DO van chân không của kênh 1…8 — hai dải rời: Y006…Y010 rồi Y021…Y023.</summary>
        static constexpr int DoVacuum(int nozzle) { return nozzle <= 5 ? DoVacuumFirst + nozzle - 1 : DoVacuumSixth + nozzle - 6; }

        /// <summary>DO van thổi của kênh 1…8 (Y024…Y031).</summary>
        static constexpr int VacuumBlower(int nozzle) { return VacuumBlowerFirst + nozzle - 1; }

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
                n[BtnStart2] = "START button #2";
                n[DiLaserReady] = "Laser ready";
                n[DiLaserBusy] = "Laser busy";
                n[DiLaserEnd] = "Laser end";
                for (int tray = 1; tray <= TrayCount; ++tray)
                    n[TrayPresent(tray)] = std::format("Tray {} sensor", tray);
                n[LightCurtainOk] = "Photo safety sensor OK";
                for (int k = 1; k <= NozzleCount; ++k)
                    n[VacuumSensor(k)] = std::format("Vacuum sensor N{}", k);
                n[VacuumSensor(VacuumChannelCount)] = "(spare) Vacuum sensor N8";
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
                n[DoLampStartBtn2] = "START button #2 lamp";
                n[LaserStart] = "Laser start";
                n[LaserStop] = "Laser stop";
                for (int k = 1; k <= NozzleCount; ++k)
                {
                    n[DoVacuum(k)] = std::format("Solenoid vacuum N{}", k);
                    n[VacuumBlower(k)] = std::format("Vacuum blower N{}", k);
                }
                n[DoVacuum(VacuumChannelCount)] = "(spare) Solenoid vacuum N8";
                n[VacuumBlower(VacuumChannelCount)] = "(spare) Vacuum blower N8";
                n[DoTowerLampRed] = "Tower lamp red";
                n[DoTowerLampYellow] = "Tower lamp yellow";
                n[DoTowerLampGreen] = "Tower lamp green";
                n[DoBuzzer] = "Buzzer";
                n[DoLightCam] = "CAM light";
                n[ZAxisBrakeRelease] = "Z axis brake release";
                return n;
            }();
            return names;
        }
    };
}
