#pragma once
#include <memory>
#include <string>
#include <vector>

#include "Base/Event.h"
#include "EtherCat/Soem/RtCyclicThread.h"
#include "EtherCat/Transport/IEtherCatBus.h"

namespace ecm
{
    /// <summary>Một card mạng của máy, theo đúng cách Npcap đặt tên thiết bị.</summary>
    struct NetworkAdapterInfo
    {
        /// <summary><c>\Device\NPF_{GUID}</c> — giá trị ghi vào cấu hình và đưa cho SOEM.</summary>
        std::string DeviceName;
        /// <summary>Tên kết nối trong Windows, vd. "Ethernet 2".</summary>
        std::string FriendlyName;
        /// <summary>Tên card, vd. "Intel(R) Ethernet Controller I226-V".</summary>
        std::string Description;
    };

    /// <summary>
    /// IEtherCatBus chạy trên SOEM v2.0.0 (ThirdParty\soem), nói chuyện với slave thật qua một card
    /// mạng Windows bằng Npcap. Port native từ lớp C++/CLI cùng tên của thư viện EtherCatMaster:
    /// toàn bộ trình tự bring-up, cách dò PDO và cách xử lý bộ ghép modular (Leadshine R1EC) giữ
    /// nguyên vì đã chạy đúng trên phần cứng thật.
    ///
    /// <para>Khác biệt duy nhất về bản chất là VÒNG PDO: bản cũ phát/nhận khung trong callback của
    /// timer multimedia; bản này dùng một luồng thời gian thực ghim vào core CPU dành riêng
    /// (<see cref="rt::RtCyclicThread"/>), và luồng app không còn đụng thẳng vào IOmap của SOEM mà đi
    /// qua vùng đệm không khoá (rt::ProcessImage).</para>
    ///
    /// <para>Offset PDO được dò SỐNG từ từ điển CoE của từng slave lúc mở bus (0x1C12/0x1C13 +
    /// 0x1600/0x1A00), không viết cứng theo model driver — mạng lẫn nhiều hãng driver CiA402 chạy
    /// được mà không sửa code, miễn mỗi driver map các đối tượng chuẩn vào PDO mặc định.</para>
    /// </summary>
    class SoemEtherCatBus final : public IEtherCatBus
    {
    public:
        SoemEtherCatBus();
        ~SoemEtherCatBus() override;
        SoemEtherCatBus(const SoemEtherCatBus&) = delete;
        SoemEtherCatBus& operator=(const SoemEtherCatBus&) = delete;

        /// <summary>Npcap (wpcap.dll) có nạp được không. Bus thật không mở được nếu thiếu — và KHÔNG
        /// bao giờ lặng lẽ rơi về mô phỏng.</summary>
        static bool IsNpcapAvailable(std::string* error = nullptr);

        /// <summary>Danh sách card mạng (trừ loopback). Không cần Npcap để liệt kê.</summary>
        static std::vector<NetworkAdapterInfo> ListAdapters();

        /// <summary>Cấu hình luồng chu kỳ (chu kỳ, core dành riêng, quay chờ). Có hiệu lực từ lần mở
        /// bus kế tiếp.</summary>
        void SetRealtimeOptions(const rt::RtOptions& options);
        rt::RtOptions RealtimeOptions() const;

        /// <summary>Thống kê đầy đủ của luồng chu kỳ (GetCyclicStats chỉ trả phần giao diện chung có).</summary>
        rt::RtStats GetRtStats() const;

        /// <summary>Một dòng mô tả cấu hình thực tế của luồng chu kỳ ở lần mở bus gần nhất.</summary>
        std::string RealtimeInfo() const;

        /// <summary>Lý do lần Open/ConnectAllAxis/ScanSlaves gần nhất không mở được card mạng; rỗng nếu mở được.</summary>
        std::string LastOpenError() const;

        /// <summary>Thông báo của tầng bus (cấu hình luồng chu kỳ, thiếu Npcap…). Gọi trên luồng đang mở bus.</summary>
        base::Event<const std::string&> OnLog;

        // ── IEtherCatBus ──
        bool Open(const std::string& networkAdapter, int expectedSlaveCount) override;
        std::vector<AxisConnectResult> ConnectAllAxis(const std::string& networkAdapter,
                                                      const std::vector<AxisConfig>& axes,
                                                      const std::vector<IoModuleConfig>& ioModules,
                                                      std::vector<AxisConnectResult>& ioResults) override;
        std::vector<DiscoveredSlaveInfo> ScanSlaves(const std::string& networkAdapter) override;
        void Close() override;
        int SlaveCount() const override;
        void CyclicExchange() override;

        void WriteSdo(int slavePosition, uint16_t index, uint8_t subIndex, int value, uint8_t sizeBytes) override;
        int ReadSdo(int slavePosition, uint16_t index, uint8_t subIndex, uint8_t sizeBytes) override;
        bool TryReadSdo(int slavePosition, uint16_t index, uint8_t subIndex, uint8_t sizeBytes, int& value) override;

        void SetControlword(int slavePosition, uint16_t controlword) override;
        void SetModeOfOperation(int slavePosition, int8_t mode) override;
        void SetTargetPosition(int slavePosition, int counts) override;
        void SetProfileVelocity(int slavePosition, uint32_t countsPerSec) override;
        void SetProfileAcceleration(int slavePosition, uint32_t countsPerSec2) override;
        void SetProfileDeceleration(int slavePosition, uint32_t countsPerSec2) override;

        uint16_t GetStatusword(int slavePosition) override;
        int8_t GetModeOfOperationDisplay(int slavePosition) override;
        int GetActualPosition(int slavePosition) override;
        int GetActualVelocity(int slavePosition) override;
        int16_t GetActualTorque(int slavePosition) override;
        uint16_t GetErrorCode(int slavePosition) override;

        uint64_t GetDigitalInputs(int slavePosition) override;
        void SetDigitalOutputs(int slavePosition, uint64_t mask) override;
        uint32_t GetIoModuleDeviceStatus(int slavePosition) override;
        std::vector<uint32_t> GetDetectedModuleIdents(int slavePosition) override;

        int GetIoSlotCount(int slavePosition) override;
        uint32_t GetIoSlotIdent(int slavePosition, int slot) override;
        int GetIoSlotInputBitCount(int slavePosition, int slot) override;
        int GetIoSlotOutputBitCount(int slavePosition, int slot) override;
        uint32_t GetIoSlotInputRegister(int slavePosition, int slot) override;
        void SetIoSlotOutputRegister(int slavePosition, int slot, uint32_t value) override;

        CyclicStats GetCyclicStats() override;
        void SetModuleCatalog(const std::vector<EsiModuleInfo>& modules) override;
        void SetSyncManagerCatalog(const std::vector<EsiSyncManagerInfo>& syncManagers) override;

    private:
        // SOEM kéo theo winsock/pcap và macro `inline` — giấu hết sau Impl, không để lọt ra header.
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
