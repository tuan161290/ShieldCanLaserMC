#include "Utility/Log/LogWorker.h"

#include "Base/Async.h"
#include "Base/Text.h"

#include <share.h>
#include <windows.h>

namespace scs
{
    namespace fs = std::filesystem;

    LogWorker& LogWorker::Instance()
    {
        static LogWorker instance;
        return instance;
    }

    LogWorker::~LogWorker()
    {
        Shutdown();
    }

    void LogWorker::Init(const fs::path& rootPath)
    {
        std::lock_guard<std::mutex> life(m_lifeMutex);
        {
            std::lock_guard<std::mutex> lock(m_fileMutex);
            m_rootPath = rootPath;
            m_hasRoot = true;
        }
        if (m_running.load()) return;

        m_running.store(true);
        m_writer = std::jthread([this](std::stop_token st) { WriteLoop(st); });
    }

    void LogWorker::RecordLog(std::string_view message, LogLevel level, std::string_view source)
    {
        if (level < m_minLevel.load()) return;

        LogData entry;
        entry.UpdateTime = std::chrono::system_clock::now();
        entry.Level = level;
        entry.Source = std::string(source);
        entry.Message = std::string(message);

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queue.push_back(entry);
            ++m_pending;
            m_signalled = true;
        }
        m_signal.notify_one();

        {
            // Chèn ở đầu — mới nhất lên trên, khỏi phải cuộn xuống mới thấy dòng vừa ghi.
            // Dòng cũ nhất nằm ở CUỐI danh sách nên cắt bớt cũng từ cuối.
            std::lock_guard<std::mutex> lock(m_listMutex);
            m_list.push_front(std::move(entry));
            int max = m_maxLines.load();
            while ((int)m_list.size() > max && !m_list.empty())
                m_list.pop_back();
        }
        ++m_version;
    }

    std::vector<LogData> LogWorker::LogList() const
    {
        std::lock_guard<std::mutex> lock(m_listMutex);
        return std::vector<LogData>(m_list.begin(), m_list.end());
    }

    void LogWorker::Flush()
    {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_signalled = true;
        }
        m_signal.notify_one();

        // Thread nền ghi theo lô; chờ ngắn là đủ cho hàng đợi cỡ vài trăm dòng.
        for (int i = 0; i < 50; i++)
        {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (m_pending == 0) break;
            }
            base::Delay(10);
        }

        std::lock_guard<std::mutex> lock(m_fileMutex);
        if (m_stream) fflush(m_stream);
    }

    void LogWorker::Shutdown()
    {
        std::lock_guard<std::mutex> life(m_lifeMutex);
        m_running.store(false);
        if (m_writer.joinable())
        {
            m_writer.request_stop();
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_signalled = true;
            }
            m_signal.notify_all();
            m_writer.join();
        }

        std::lock_guard<std::mutex> lock(m_fileMutex);
        CloseStream();
    }

    void LogWorker::WriteLoop(std::stop_token st)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        while (!st.stop_requested())
        {
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_signal.wait_for(lock, std::chrono::milliseconds(500), [&] { return m_signalled || st.stop_requested(); });
                m_signalled = false;
            }
            DrainQueue();
        }
        DrainQueue();
    }

    void LogWorker::DrainQueue()
    {
        bool wrote = false;

        while (true)
        {
            LogData entry;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (m_queue.empty()) break;
                entry = std::move(m_queue.front());
                m_queue.pop_front();
            }

            try
            {
                std::lock_guard<std::mutex> lock(m_fileMutex);
                EnsureStream(entry.UpdateTime);
                if (m_stream)
                {
                    std::string line = entry.ToLine() + "\r\n";
                    fwrite(line.data(), 1, line.size(), m_stream);
                }
                wrote = true;
            }
            catch (...)
            {
                // Ghi log mà ném exception thì không có chỗ nào để báo — nuốt và bỏ dòng đó.
                // Đĩa đầy / mất quyền ghi được phát hiện riêng bằng alarm 1003 (SafetyMonitor).
            }

            std::lock_guard<std::mutex> lock(m_queueMutex);
            --m_pending;
        }

        if (wrote)
        {
            std::lock_guard<std::mutex> lock(m_fileMutex);
            if (m_stream) fflush(m_stream);
        }
    }

    void LogWorker::CloseStream()
    {
        if (!m_stream) return;
        fflush(m_stream);
        fclose(m_stream);
        m_stream = nullptr;
    }

    /// <summary>Mở file của ngày tương ứng, tự sang file mới khi qua nửa đêm.</summary>
    void LogWorker::EnsureStream(TimePoint when)
    {
        if (!m_hasRoot) return;
        int date = LocalDateKey(when);
        if (m_stream != nullptr && m_streamDate == date) return;

        CloseStream();

        fs::path dir = m_rootPath / "Log" / FormatLocal(when, "yyyyMMdd");
        fs::create_directories(dir);
        fs::path file = dir / "app.log";

        // BOM chỉ ghi khi tạo file mới. Không có BOM thì Notepad và các công cụ Windows đọc theo
        // bảng mã ANSI và tiếng Việt thành ký tự rác; ghi BOM mỗi lần mở để ghi nối lại chèn rác
        // vào giữa file.
        std::error_code ec;
        bool isNew = !fs::exists(file, ec) || fs::file_size(file, ec) == 0;

        // Ghi nối, cho process khác đọc/ghi cùng lúc (FileShare.ReadWrite).
        m_stream = _wfsopen(file.c_str(), L"ab", _SH_DENYNO);
        if (!m_stream) return;
        if (isNew) fwrite("\xEF\xBB\xBF", 1, 3, m_stream);
        m_streamDate = date;
    }

    std::vector<LogData> LogWorker::ReadDay(const fs::path& rootPath, TimePoint day)
    {
        std::vector<LogData> result;
        fs::path file = rootPath / "Log" / FormatLocal(day, "yyyyMMdd") / "app.log";
        std::error_code ec;
        if (!fs::exists(file, ec)) return result;

        try
        {
            FILE* f = _wfsopen(file.c_str(), L"rb", _SH_DENYNO);
            if (!f) return result;

            std::string text;
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);

            size_t pos = base::StartsWith(text, "\xEF\xBB\xBF") ? 3 : 0;
            while (pos < text.size())
            {
                size_t end = text.find_first_of("\r\n", pos);
                if (end == std::string::npos) end = text.size();

                LogData d;
                if (LogData::TryParseLine(std::string_view(text).substr(pos, end - pos), d))
                    result.push_back(std::move(d));

                pos = end;
                if (pos < text.size() && text[pos] == '\r') ++pos;
                if (pos < text.size() && text[pos] == '\n') ++pos;
            }
        }
        catch (...) { }

        return result;
    }
}
