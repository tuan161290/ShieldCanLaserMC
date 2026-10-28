#pragma once
// Dành riêng một core CPU cho luồng chu kỳ EtherCAT.
//
// Cơ chế: Windows CPU Sets. Tiến trình đặt "CPU set mặc định" = mọi CPU logic TRỪ core được chọn —
// mọi luồng của app (UI, .NET/VisionMaster, log, routine, kể cả luồng do thư viện bên thứ ba tạo
// sau này) tự động rời core đó. Riêng luồng chu kỳ chọn đích danh core đó. Khác với affinity mask
// cứng, CPU set của tiến trình áp cho cả những luồng không do ta tạo ra, và không cần quyền admin.
//
// Giới hạn: CPU Sets chỉ điều khiển tiến trình NÀY. Tiến trình khác vẫn có thể được xếp lên core đó
// khi luồng chu kỳ đang ngủ; luồng chu kỳ chạy mức TIME_CRITICAL nên giành lại ngay khi tới hạn.
// Muốn cả hệ điều hành tránh core đó thì đặt ReservedCpuSets trong registry (xem tài liệu bàn giao).
#include <cstdint>
#include <string>
#include <vector>

namespace ecm::rt
{
    struct LogicalCpu
    {
        uint32_t CpuSetId{};
        uint16_t Group{};
        uint8_t  LogicalIndex{};      // chỉ số trong group
        uint8_t  CoreIndex{};         // các CPU logic chung một core vật lý (SMT) có cùng số này
        uint8_t  EfficiencyClass{};   // lớn hơn = core mạnh hơn (P-core trên CPU lai)
        bool     Parked{};
        bool     Allocated{};         // đang được dành riêng cho một tiến trình nào đó
        bool     AllocatedToThisProcess{};
    };

    std::vector<LogicalCpu> EnumerateCpus();

    struct CoreReservation
    {
        /// <summary>CPU set mặc định của tiến trình đã loại core này ra.</summary>
        bool Reserved{};
        /// <summary>CPU logic luồng chu kỳ sẽ chạy; -1 = không chọn được (máy quá ít core).</summary>
        int LogicalIndex{ -1 };
        uint16_t Group{};
        uint32_t CpuSetId{};
        /// <summary>Mọi CPU logic của core vật lý đó — CPU "anh em" SMT cũng bị bỏ trống, vì hai luồng
        /// chung một core vật lý giành nhau tài nguyên thực thi và gây jitter y như chung một CPU.</summary>
        std::vector<uint32_t> CoreCpuSetIds;
        /// <summary>Một dòng mô tả để ghi log.</summary>
        std::string Description;
    };

    /// <summary>
    /// Chọn core và đẩy phần còn lại của tiến trình ra khỏi nó.
    /// <paramref name="requestedLogicalIndex"/> = -1: tự chọn — core số cao nhất trong lớp hiệu năng
    /// THẤP nhất (E-core trên CPU lai; CPU thường thì là core cuối), không phải core chứa CPU 0
    /// (CPU 0 gánh nhiều ngắt/DPC nhất của hệ thống).
    /// Tự chọn chỉ dành riêng khi máy có từ 4 CPU logic trở lên; chỉ định đích danh thì cần từ 2.
    /// </summary>
    CoreReservation ReserveCore(int requestedLogicalIndex);

    /// <summary>Trả CPU set mặc định của tiến trình về "tất cả".</summary>
    void ReleaseCore(const CoreReservation& reservation);

    /// <summary>Ghim luồng đang gọi vào CPU đã chọn. Gọi từ chính luồng chu kỳ.</summary>
    bool PinCurrentThread(const CoreReservation& reservation);

    /// <summary>
    /// Tắt hai cơ chế tiết kiệm điện của Windows 11 làm hỏng định thời: (1) bỏ qua yêu cầu độ phân
    /// giải timer khi cửa sổ app bị che/thu nhỏ — đúng lúc operator mở cửa sổ khác đè lên HMI;
    /// (2) EcoQoS hạ xung/đẩy luồng sang core tiết kiệm điện. Gọi một lần, không cần hoàn tác.
    /// </summary>
    void DisableProcessThrottling();

    /// <summary>CPU logic (trong group) luồng đang gọi đang chạy trên đó.</summary>
    int CurrentCpuIndex();
}
