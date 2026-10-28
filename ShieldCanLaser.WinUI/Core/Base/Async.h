#pragma once
// Thay cho Task/async-await của bản C#: mọi thao tác dài là HÀM CHẶN chạy trên luồng nền, huỷ bằng
// std::stop_token (vai trò của CancellationToken). Luồng UI không bao giờ gọi thẳng các hàm này —
// nó đẩy việc sang luồng nền (RunDetached/RunAsync) rồi đọc trạng thái theo chu kỳ.
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stop_token>
#include <vector>

namespace base
{
    /// <summary>Tương đương OperationCanceledException: routine bắt cái này để phân biệt "bị dừng"
    /// với "lỗi".</summary>
    struct OperationCanceled : std::exception
    {
        const char* what() const noexcept override { return "operation canceled"; }
    };

    inline void ThrowIfCancelled(const std::stop_token& ct)
    {
        if (ct.stop_requested()) throw OperationCanceled();
    }

    /// <summary>Task.Delay(ms, ct): ngủ, ném OperationCanceled ngay khi token bị huỷ.</summary>
    void Delay(int ms, const std::stop_token& ct);

    /// <summary>Task.Delay(ms) / Thread.Sleep(ms).</summary>
    void Delay(int ms);

    /// <summary>Như Delay(ms, ct) nhưng không ném: trả false nếu bị huỷ trước khi hết giờ.</summary>
    bool DelayNoThrow(int ms, const std::stop_token& ct);

    /// <summary>
    /// CancellationTokenSource.CreateLinkedTokenSource: token của nó bị huỷ khi MỘT trong các token
    /// cha bị huỷ, hoặc khi gọi Cancel(). Không copy được vì stop_callback giữ con trỏ về chính nó.
    /// </summary>
    class LinkedStopSource
    {
    public:
        explicit LinkedStopSource(const std::stop_token& a, const std::stop_token& b = {});
        LinkedStopSource(const LinkedStopSource&) = delete;
        LinkedStopSource& operator=(const LinkedStopSource&) = delete;

        std::stop_token Token() const { return m_source.get_token(); }
        void Cancel() { m_source.request_stop(); }
        bool IsCancellationRequested() const { return m_source.stop_requested(); }

    private:
        struct Relay { std::stop_source* target; void operator()() const { target->request_stop(); } };
        std::stop_source m_source;
        std::unique_ptr<std::stop_callback<Relay>> m_a, m_b;
    };

    /// <summary>Task.Run không chờ kết quả. Exception lọt ra được nuốt và báo qua
    /// <see cref="SetBackgroundErrorHandler"/> — một luồng nền chết không được kéo sập app.</summary>
    void RunDetached(std::function<void()> fn);

    /// <summary>Nơi nhận lỗi của RunDetached (app gắn vào log). Mặc định: OutputDebugString.</summary>
    void SetBackgroundErrorHandler(std::function<void(const char* what)> handler);

    /// <summary>Số việc RunDetached còn đang chạy — lúc tắt app chờ về 0 trước khi huỷ tài nguyên.</summary>
    int  BackgroundTaskCount();
    bool WaitBackgroundIdle(int timeoutMs);

    /// <summary>Task.Run có kết quả: chạy trên luồng riêng, lấy kết quả bằng future.get().</summary>
    template <class F>
    auto RunAsync(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>>
    {
        return std::async(std::launch::async, std::forward<F>(fn));
    }

    /// <summary>
    /// Task.WhenAll cho các việc trả bool: chạy đồng thời (việc đầu trên luồng gọi), chờ TẤT CẢ xong,
    /// trả true nếu mọi việc đều true. Exception đầu tiên được ném lại sau khi mọi việc đã dừng —
    /// không bao giờ bỏ lại một luồng còn đang ra lệnh cho trục.
    /// </summary>
    bool WhenAll(std::vector<std::function<bool()>> jobs);

    /// <summary>Stopwatch.StartNew().ElapsedMilliseconds.</summary>
    class Stopwatch
    {
    public:
        Stopwatch() : m_start(std::chrono::steady_clock::now()) {}
        void Restart() { m_start = std::chrono::steady_clock::now(); }
        long long ElapsedMilliseconds() const
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count();
        }
        double ElapsedSeconds() const
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
        }

    private:
        std::chrono::steady_clock::time_point m_start;
    };
}
