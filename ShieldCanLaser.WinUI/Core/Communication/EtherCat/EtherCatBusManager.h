#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Data/SystemData.h"
#include "EtherCat/Models/AxisConnectResult.h"
#include "EtherCat/Models/CyclicStats.h"
#include "EtherCat/Models/DiscoveredSlaveInfo.h"
#include "EtherCat/Models/EtherCatConfig.h"
#include "EtherCat/Services/EtherCatMasterService.h"
#include "EtherCat/Soem/RtCyclicThread.h"
#include "EtherCat/Transport/IEtherCatBus.h"

namespace ecm { class SoemEtherCatBus; }

namespace scs
{
    /// <summary>
    /// Chủ sở hữu DUY NHẤT của bus EtherCAT — 14 trục và Remote IO R1EC chia sẻ CHUNG một
    /// <c>ecm::EtherCatMasterService</c>, không phải 15 kết nối độc lập. Port từ Partron CAM TEST.
    ///
    /// <para><c>IAxis::Connect</c> vốn thiết kế cho phần cứng mỗi trục một kết nối riêng, còn
    /// EtherCAT là MỘT bus: <c>ConnectAllAxis</c> xác nhận rồi mở cả bus trong MỘT lần gọi, và chỉ mở
    /// khi MỌI slot cấu hình đều đạt (đúng vị trí + đúng Vendor/Product) VÀ số slave trên mạng đúng bằng số
    /// slot đã gán driver. <see cref="EnsureConnected"/> dung hoà hai điều đó: idempotent,
    /// lần gọi đầu tiên thật sự mở bus, các lần sau chỉ trả cờ. Cấu hình được DỰNG LẠI từ
    /// <c>SystemData</c> hiện tại ở mỗi lần mở — nút Connect All Axis ở trang Setting áp dụng
    /// ngay tham số vừa sửa (kể cả tham số luồng thời gian thực Rt*), không cần khởi động lại app.</para>
    ///
    /// <para>Ba chế độ theo <c>SystemData::NetworkAdapter</c>: <c>\Device\NPF_{GUID}</c> = SOEM
    /// thật qua Npcap; <see cref="SimulatedAdapterSentinel"/> = <c>SimulatedEtherCatBus</c> (chạy đúng
    /// đường code EtherCAT mà không cần phần cứng); rỗng = chưa chọn card mạng, bus không mở được.</para>
    /// </summary>
    class EtherCatBusManager
    {
    public:
        /// <summary>Giá trị <c>NetworkAdapter</c> chọn bus mô phỏng của thư viện EtherCAT.</summary>
        static constexpr const char* SimulatedAdapterSentinel = "SIMULATED";

        explicit EtherCatBusManager(std::function<std::shared_ptr<SystemData>()> sys);

        /// <summary>Dùng một bus cho sẵn thay vì chọn theo <c>NetworkAdapter</c> — cho kiểm thử các nhánh mà
        /// bus mô phỏng chuẩn không tạo ra được (vd. số slave trên mạng lệch cấu hình). App không dùng.</summary>
        EtherCatBusManager(std::function<std::shared_ptr<SystemData>()> sys,
                           std::shared_ptr<ecm::IEtherCatBus> bus, std::string busDescription);

        ~EtherCatBusManager();
        EtherCatBusManager(const EtherCatBusManager&) = delete;
        EtherCatBusManager& operator=(const EtherCatBusManager&) = delete;

        bool IsConnected() const { return m_service != nullptr && m_service->IsConnected(); }
        const std::string& BusDescription() const { return m_busDescription; }

        /// <summary>
        /// Tăng mỗi lần bus mở thành công. Trục ghi lại số này lúc về gốc xong và bỏ cờ đã-về-gốc khi số đổi —
        /// Connect All Axis đóng rồi mở lại bus trong cùng một lệnh, vòng quét có thể không kịp thấy khoảnh khắc
        /// mất kết nối, trong khi driver có thể đã khởi động lại (mất vị trí).
        /// </summary>
        int ConnectionEpoch() const { return m_epoch.load(); }

        /// <summary>Lý do lần mở bus gần nhất thất bại; rỗng nếu chưa thử hoặc đã mở được.</summary>
        std::string LastError() const;

        std::vector<ecm::AxisConnectResult> LastAxisResults() const;
        std::vector<ecm::AxisConnectResult> LastIoResults() const;

        /// <summary>Số slave lần mở bus gần nhất thấy trên mạng; -1 nếu lần đó không mở được bus.</summary>
        int LastFoundSlaveCount() const { return m_lastFoundSlaveCount.load(); }

        /// <summary>
        /// Số slave mạng EtherCAT phải có = số vị trí slave khác nhau của các slot đã gán driver (trục + Remote
        /// IO) — hai trục của một driver WE2x tính một slave. Slot chưa gán (ProductCode = 0) là slot không dùng —
        /// ConnectAllAxis bỏ qua nó, nên cũng không tính ở đây.
        /// </summary>
        static int ExpectedSlaveCount(const SystemData& sys);

        /// <summary>Cho EtherCatAxis/EtherCatIoDevice đọc trạng thái và gọi lệnh. nullptr nếu bus không dựng
        /// được (xem <see cref="BusDescription"/>).</summary>
        std::shared_ptr<ecm::EtherCatMasterService> Service() const { return m_service; }

        /// <summary>
        /// Idempotent: đã kết nối thì trả về true ngay; đã thử trong lượt này mà hỏng thì trả false ngay.
        /// Chưa thử thì dựng lại EtherCatConfig từ SystemData HIỆN TẠI rồi gọi ConnectAllAxis MỘT LẦN cho
        /// cả 14 trục + Remote IO, sau đó đối chiếu số slave trên mạng với <see cref="ExpectedSlaveCount"/>.
        /// </summary>
        bool EnsureConnected();

        /// <summary>Đóng rồi mở lại bus với cấu hình hiện tại — nút Connect All Axis ở trang Setting.</summary>
        bool Reconnect();

        /// <summary>An toàn gọi nhiều lần — vòng DisconnectAll của MotionConnect gọi một lần cho mỗi trục.</summary>
        void Disconnect();

        /// <summary>Quét bus (chỉ đọc định danh, không vào OP) — nút Scan ở trang Setting. Không dùng được
        /// trong lúc bus đang mở: SOEM chỉ có một ngữ cảnh (ném std::runtime_error).</summary>
        std::vector<ecm::DiscoveredSlaveInfo> ScanSlaves();

        /// <summary>Thống kê chu kỳ native (jitter, thời gian quét khung PDO). SampleCount = 0 khi bus mô phỏng.</summary>
        ecm::CyclicStats CyclicStats() const;

        /// <summary>Thống kê đầy đủ của luồng chu kỳ thời gian thực; toàn 0 khi bus không phải SOEM.</summary>
        ecm::rt::RtStats RtStats() const;

        /// <summary>Một dòng mô tả cấu hình thực tế của luồng chu kỳ ở lần mở bus gần nhất; rỗng khi bus
        /// không phải SOEM.</summary>
        std::string RealtimeInfo() const;

        /// <summary>Thư mục ESI cạnh exe (<c>DriverInfo</c>).</summary>
        static std::filesystem::path DriverInfoDir();

        void Dispose();

    private:
        static bool TryResolveBus(const std::string& networkAdapter, int ioSlave,
                                  std::shared_ptr<ecm::IEtherCatBus>& bus, std::string& error);
        static ecm::EtherCatConfig BuildConfig(const SystemData& sys);
        /// <summary>Đẩy RtCycleUs/RtCpuIndex/RtReserveCore/RtSpinUs của SystemData xuống bus SOEM — gọi
        /// trước MỖI lần mở bus (có hiệu lực từ lần mở kế tiếp).</summary>
        void ApplyRealtimeOptions(const SystemData& sys);
        void SetLastError(const std::string& error);
        /// <summary>Dựng service trên m_bus và nối log của bus/service vào LogWorker.</summary>
        void AttachBus();

        std::function<std::shared_ptr<SystemData>()> m_sys;
        std::recursive_mutex m_gate;
        std::shared_ptr<ecm::EtherCatMasterService> m_service;
        /// <summary>Giữ riêng để đọc SlaveCount() và các hàm riêng của bus SOEM.</summary>
        std::shared_ptr<ecm::IEtherCatBus> m_bus;
        std::shared_ptr<ecm::SoemEtherCatBus> m_soem;   // != nullptr chỉ khi m_bus là bus SOEM thật
        std::string m_busResolutionError;
        std::string m_busDescription;

        mutable std::mutex m_stateMutex;                 // m_lastError, m_lastAxisResults, m_lastIoResults
        std::string m_lastError;
        std::vector<ecm::AxisConnectResult> m_lastAxisResults;
        std::vector<ecm::AxisConnectResult> m_lastIoResults;
        std::atomic<int> m_lastFoundSlaveCount{ -1 };
        std::atomic<int> m_epoch{ 0 };

        /// <summary>Đã thử mở bus trong lượt kết nối này chưa — 14 trục + Remote IO cùng gọi
        /// <see cref="EnsureConnected"/>, bus lỗi thì không được thử lại 10 lần liên tiếp (mỗi lần SOEM thật
        /// mất vài giây chờ slave). <see cref="Reconnect"/> mở lượt mới.</summary>
        bool m_attempted{ false };
    };
}
