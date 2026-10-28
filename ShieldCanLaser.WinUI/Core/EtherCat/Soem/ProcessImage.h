#pragma once
// Vùng đệm process-image giữa luồng chu kỳ (thời gian thực) và phần còn lại của app.
//
// Bản C++/CLI cho luồng app đọc/ghi THẲNG vào IOmap của SOEM trong lúc callback timer đang phát/nhận
// khung, không khoá ("ghi một word đã căn chỉnh là nguyên tử trên x64"). Điều đó đúng với từng word
// căn chỉnh, nhưng offset PDO do driver quyết định: một Target Position 4 byte nằm vắt qua ranh giới
// cache line có thể được phát đi nửa cũ nửa mới. Ở đây IOmap thuộc RIÊNG luồng chu kỳ:
//
//   Ngõ ra (app → bus): luồng app xếp lệnh ghi {offset, độ dài, dữ liệu} vào một hàng đợi vòng không
//     khoá; đầu mỗi chu kỳ luồng chu kỳ rút hết hàng đợi vào IOmap rồi mới phát khung. Một lệnh ghi
//     hoặc đã vào trọn vẹn, hoặc chưa vào — không bao giờ vào một nửa. Các luồng app tự xếp hàng với
//     nhau bằng mutex; luồng chu kỳ không bao giờ chờ mutex đó.
//
//   Ngõ vào (bus → app): sau khi nhận khung, luồng chu kỳ chép vùng ngõ vào sang một bản chụp có số
//     thứ tự (seqlock). Bên đọc chép trường mình cần rồi kiểm số thứ tự, trúng lúc đang ghi thì đọc
//     lại. Luồng chu kỳ là bên ghi duy nhất và không bao giờ chờ bên đọc.
//
// Hệ quả: luồng chu kỳ không có điểm nào phải chờ một luồng mức ưu tiên thường — không thể có đảo
// ưu tiên giữa vòng PDO và UI/routine/SDO.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ecm::rt
{
    class ProcessImage
    {
    public:
        /// <summary>Gắn với IOmap của SOEM. Gọi khi bus đã vào OPERATIONAL và TRƯỚC khi luồng chu kỳ
        /// chạy; trạng thái ngõ ra hiện có trong IOmap được lấy làm điểm xuất phát.</summary>
        void Attach(uint8_t* outputs, size_t outputBytes, const uint8_t* inputs, size_t inputBytes);

        /// <summary>Tách khỏi IOmap (bus đóng). Chỉ gọi sau khi luồng chu kỳ đã dừng hẳn.</summary>
        void Detach();

        bool Attached() const { return m_outputs != nullptr || m_inputs != nullptr; }

        // ── Phía app (luồng bất kỳ) ──

        /// <summary>Ghi <paramref name="length"/> byte (tối đa 8) vào vùng ngõ ra tại offset tính từ đầu
        /// vùng ngõ ra của group. Có hiệu lực từ khung PDO kế tiếp.</summary>
        void WriteOutput(size_t offset, const void* data, size_t length);

        /// <summary>Giá trị ngõ ra app đã đặt gần nhất (kể cả lệnh còn nằm trong hàng đợi) — dùng cho
        /// đọc-sửa-ghi mặt nạ DO.</summary>
        void ReadOutputShadow(size_t offset, void* data, size_t length) const;

        /// <summary>Đọc nhất quán một trường ngõ vào từ bản chụp của chu kỳ gần nhất.</summary>
        void ReadInput(size_t offset, void* data, size_t length) const;

        template <class T>
        T Input(size_t offset) const
        {
            T value{};
            ReadInput(offset, &value, sizeof(T));
            return value;
        }

        template <class T>
        void Output(size_t offset, T value)
        {
            WriteOutput(offset, &value, sizeof(T));
        }

        // ── Phía luồng chu kỳ ──

        /// <summary>Rút hàng đợi lệnh ghi vào IOmap. Gọi ngay trước ecx_send_processdata.</summary>
        void ApplyPendingOutputs();

        /// <summary>Chép vùng ngõ vào của IOmap sang bản chụp. Gọi ngay sau ecx_receive_processdata.</summary>
        void PublishInputs();

    private:
        struct Write
        {
            uint16_t Offset;
            uint8_t Length;
            uint8_t Data[8];
        };

        static constexpr uint32_t QueueSize = 1024;   // luỹ thừa của 2
        static constexpr uint32_t QueueMask = QueueSize - 1;

        uint8_t* m_outputs{};
        size_t m_outputBytes{};
        const uint8_t* m_inputs{};
        size_t m_inputBytes{};

        // Hàng đợi một-bên-ghi (các luồng app, nối tiếp nhau bằng m_writerMutex) / một-bên-đọc (luồng chu kỳ).
        mutable std::mutex m_writerMutex;
        std::vector<uint8_t> m_outputShadow;
        Write m_queue[QueueSize]{};
        std::atomic<uint32_t> m_head{ 0 };   // luồng chu kỳ tiến
        std::atomic<uint32_t> m_tail{ 0 };   // luồng app tiến

        // Bản chụp ngõ vào (seqlock).
        std::atomic<uint32_t> m_inputSeq{ 0 };
        std::vector<uint8_t> m_inputSnapshot;
    };
}
