#pragma once
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "Communication/MotionConnect.h"
#include "Communication/VmContract.h"
#include "Communication/VmOutCheck.h"
#include "Data/Model/ModelData.h"
#include "Data/SystemData.h"

namespace scs
{
    /// <summary>Kết quả cổng hiệu chuẩn phía App (<see cref="VmCycle::EvaluateCalib"/>). Chỉ đọc model của App —
    /// hằng số hiệu chuẩn không có bản sao nào trong solution để đối chiếu.</summary>
    struct VmCalibGate
    {
        /// <summary>Lý do chặn (alarm 4011); rỗng là không có.</summary>
        std::string Block;
        /// <summary>Số ngày tính từ lần đo cũ nhất trong bản ghi; NaN nếu chưa có lần nào.</summary>
        double DaysSinceOldest = std::numeric_limits<double>::quiet_NaN();

        bool Ready() const { return Block.empty(); }
        std::string Describe() const;
    };

    /// <summary>Kết quả đọc một lần gọi Flow LocateCan.</summary>
    struct VmCameraRead
    {
        bool CallOk{ false };
        std::string CallError;
        /// <summary>Echo (RunId, trục vừa ghi) khớp — sai là đọc nhầm Flow, nhầm biến hoặc nhầm lần chạy.</summary>
        bool EchoOk{ true };
        std::string EchoIssue;
        /// <summary>Output <c>Found</c> — nguồn duy nhất của "thấy / không thấy".</summary>
        bool Found{ false };
        /// <summary>Echo khớp và (nếu thấy) mọi output mới hợp lệ.</summary>
        bool VmValid{ false };
        std::string VmIssue;
        /// <summary>ObjX_mm / ObjY_mm — vị trí can trong hệ vật lý (X, Z) của file hiệu chuẩn.</summary>
        double X{ 0 }, Y{ 0 };
        /// <summary>Encoder X/Z App ghi vào biến lúc chụp.</summary>
        double AxX{ std::numeric_limits<double>::quiet_NaN() }, AxZ{ std::numeric_limits<double>::quiet_NaN() };
        /// <summary>Rel = Obj − encoder: vị trí can SO VỚI nozzle (PLAN-LASER.md §4.1). Chỉ có nghĩa khi Found.</summary>
        double RelX{ 0 }, RelZ{ 0 };
    };

    /// <summary>Trung bình N ảnh App lấy cho phép đo hằng số: vị trí can và vị trí trục lúc chụp.</summary>
    struct VmMeanCapture
    {
        bool Ok{ false };
        std::string Reason;
        int Shots{ 0 };
        double X{ 0 }, Y{ 0 };
        double AxX{ 0 }, AxZ{ 0 };
        double RelX{ 0 }, RelZ{ 0 };
    };

    /// <summary>Phán quyết kiểm tra lưới X × Z (<see cref="VmCycle::JudgeGrid"/>).</summary>
    struct VmGridJudge
    {
        int Status{ 0 };
        double StepX{ 0 }, StepZ{ 0 }, ErrX{ 0 }, ErrZ{ 0 }, CrossX{ 0 }, CrossZ{ 0 };
    };

    /// <summary>Độ lệch của can trên một nozzle và đích khắc đã bù (<see cref="VmCycle::Deviation"/>).</summary>
    struct VmDeviation
    {
        /// <summary>false = vượt ngưỡng bù (ô NG, không khắc) — <see cref="Why"/> nói rõ.</summary>
        bool Ok{ false };
        std::string Why;
        /// <summary>dX, dZ = Rel − Ref[k] (mm).</summary>
        double Dx{ 0 }, Dz{ 0 };
        /// <summary>Đích X, Z đã cộng Trim và Gain · d.</summary>
        double TargetX{ 0 }, TargetZ{ 0 };
    };

    /// <summary>
    /// Hợp đồng gọi Flow VisionMaster của máy Laser Marking (một camera, một Flow LocateCan) — dựng yêu cầu gọi Flow,
    /// đọc và đối chiếu kết quả, tính độ lệch + đích bù, phép kiểm lưới App tự phán và cổng hiệu chuẩn.
    /// <para>Mọi hàm là hàm thuần (không chặn), gọi được từ mọi luồng.</para>
    /// </summary>
    class VmCycle
    {
    public:
        // ───────────────────────── biến App ghi ─────────────────────────

        /// <summary>Tên biến trục trống ở trang Setting là lỗi cứng. Rỗng = không có vấn đề.</summary>
        static std::string AxisVarProblem(const VisionOutputNames* n);

        /// <summary>Vị trí ENCODER X, Z + Nozzle + RunId — ghi trước mọi Flow. motion = nullptr → mọi vị trí là 0.</summary>
        static std::map<std::string, double> AxisGlobals(const MotionConnect* motion, const VisionOutputNames& n, int runId, int nozzle);

        // ───────────────────────── yêu cầu gọi Flow ─────────────────────────

        /// <summary>LocateCan nhánh soi (biến rẽ nhánh = 1): <c>Found</c> và echo bắt buộc; output của Script đọc dạng tuỳ chọn.</summary>
        static VmCallRequest LocateRequest(const VisionOutputNames& n, const std::map<std::string, double>& globals);

        // ───────────────────────── hợp đồng Output Params ─────────────────────────

        /// <summary>Bộ output mà Flow phải khai — dựng từ CHÍNH hàm tạo lời gọi ở trên.</summary>
        static std::vector<VmFlowExpect> ExpectedFlowOutputs(const VisionOutputNames& n);

        /// <summary>Bản khai "đúng hết" của một Flow — mô phỏng dùng.</summary>
        static VmFlowOutputs FakeFlowOutputs(const std::string& flow, const VisionOutputNames& n);

        // ───────────────────────── đọc kết quả ─────────────────────────

        static VmCameraRead ReadLocate(const VmCallResult& r, const std::map<std::string, double>& sent, const VisionOutputNames& n);

        // ───────────────────────── độ lệch và đích bù ─────────────────────────

        /// <summary>
        /// Độ lệch = (RelX, RelZ) − Ref của nozzle; đích = (baseX, baseZ) + Trim + Gain · d (PLAN-LASER.md §4.3). Vượt
        /// <c>MaxCompMm</c> → Ok = false. <paramref name="useVision"/> = false → d = 0 (chỉ cộng Trim).
        /// </summary>
        static VmDeviation Deviation(const NozzleCalib& nozzle, double relX, double relZ, const LaserCompSettings& comp,
                                     double baseX, double baseZ, bool useVision = true);

        // ───────────────────────── App tự phán ─────────────────────────

        /// <summary>Kiểm tra lưới: ba lần đo can đứng yên trên nozzle (gốc, sau khi chạy X, sau khi chạy Z) — Rel phải
        /// đứng yên. Bắt sai thang đo, sai dấu, lẫn trục.</summary>
        static VmGridJudge JudgeGrid(const VmMeanCapture& s0, const VmMeanCapture& s1, const VmMeanCapture& s2, double tolMm);

        // ───────────────────────── cổng hiệu chuẩn ─────────────────────────

        /// <summary>Cổng hiệu chuẩn: kiểm tra lưới đạt, Ref của các nozzle đã đo, ngưỡng hợp lệ. <paramref name="nozzle"/>
        /// = 0 (Auto) đòi cả 7; 1…7 chỉ xét nozzle đó.</summary>
        static VmCalibGate EvaluateCalib(const ModelData* model, const SystemData* sys, int nozzle = 0);

        // ───────────────────────── tiện ích ─────────────────────────

        static double Val(const std::map<std::string, double>& d, const std::string& key);
        static std::string Fmt(double v);
    };
}
