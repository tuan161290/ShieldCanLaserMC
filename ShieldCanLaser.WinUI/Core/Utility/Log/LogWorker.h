#pragma once
#include "Utility/Log/LogData.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace scs
{
    /// <summary>
    /// Ghi log ra file theo ngày, không chặn thread gọi.
    ///
    /// <para><b>Ghi nối, không ghi đè.</b> Ghi nối từng dòng TSV: chi phí O(1) mỗi dòng và không
    /// bao giờ mất log cũ nếu app chết giữa chừng (bản HousingCorrosion serialize lại cả danh sách
    /// sau mỗi dòng — O(n²)).</para>
    ///
    /// <para>Một thread nền duy nhất rút hàng đợi và ghi file, nên thứ tự dòng trong file luôn
    /// đúng thứ tự thời gian dù nhiều thread cùng gọi <see cref="RecordLog"/>.</para>
    /// </summary>
    class LogWorker
    {
    public:
        static LogWorker& Instance();

        LogWorker(const LogWorker&) = delete;
        LogWorker& operator=(const LogWorker&) = delete;
        ~LogWorker();

        void Init(const std::filesystem::path& rootPath);

        void RecordLog(std::string_view message, LogLevel level = LogLevel::Info, std::string_view source = "System");

        /// <summary>Ép ghi hết hàng đợi xuống đĩa. Gọi trước khi thoát app hoặc khi vào EMG/ALARM.</summary>
        void Flush();

        /// <summary>Dừng thread ghi (ghi nốt hàng đợi), đóng file. Gọi lại Init() để chạy tiếp.</summary>
        void Shutdown();

        /// <summary>Bản chụp danh sách hiển thị trên trang Log — MỚI NHẤT ĐỨNG ĐẦU, tối đa
        /// <see cref="MaxInMemoryLines"/> dòng. File trên đĩa giữ đủ, không bị cắt.</summary>
        std::vector<LogData> LogList() const;

        /// <summary>Tăng mỗi lần LogList đổi — UI poll so số này để khỏi dựng lại danh sách.</summary>
        uint64_t Version() const { return m_version.load(); }

        int  MaxInMemoryLines() const { return m_maxLines.load(); }
        void SetMaxInMemoryLines(int n) { m_maxLines.store(n); }

        LogLevel MinLevel() const { return m_minLevel.load(); }
        void SetMinLevel(LogLevel level) { m_minLevel.store(level); }

        /// <summary>Đọc lại file log của một ngày — dùng cho trang Log xem lịch sử.</summary>
        static std::vector<LogData> ReadDay(const std::filesystem::path& rootPath, TimePoint day);

    private:
        LogWorker() = default;

        void WriteLoop(std::stop_token st);
        void DrainQueue();
        void EnsureStream(TimePoint when);
        void CloseStream();

        mutable std::mutex m_listMutex;          // m_list
        std::deque<LogData> m_list;
        std::atomic<uint64_t> m_version{ 0 };
        std::atomic<int> m_maxLines{ 500 };
        std::atomic<LogLevel> m_minLevel{ LogLevel::Info };

        std::mutex m_queueMutex;                 // m_queue, m_signalled, m_pending
        std::condition_variable m_signal;
        std::deque<LogData> m_queue;
        bool m_signalled{ false };
        int m_pending{ 0 };                      // đã xếp hàng nhưng chưa ghi xong

        std::mutex m_lifeMutex;                  // Init/Shutdown
        std::jthread m_writer;
        std::atomic<bool> m_running{ false };

        std::mutex m_fileMutex;                  // m_rootPath, m_stream, m_streamDate
        std::filesystem::path m_rootPath;
        bool m_hasRoot{ false };
        FILE* m_stream{ nullptr };
        int m_streamDate{ 0 };
    };
}
