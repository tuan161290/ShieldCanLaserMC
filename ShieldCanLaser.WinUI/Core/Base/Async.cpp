#include "Base/Async.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace base
{
    bool DelayNoThrow(int ms, const std::stop_token& ct)
    {
        if (ct.stop_requested()) return false;
        if (ms <= 0) return true;

        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock<std::mutex> lock(m);
        // wait_for trả true khi predicate đúng, tức là token đã bị huỷ.
        bool cancelled = cv.wait_for(lock, ct, std::chrono::milliseconds(ms), [&] { return ct.stop_requested(); });
        return !cancelled;
    }

    void Delay(int ms, const std::stop_token& ct)
    {
        if (!DelayNoThrow(ms, ct)) throw OperationCanceled();
    }

    void Delay(int ms)
    {
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    LinkedStopSource::LinkedStopSource(const std::stop_token& a, const std::stop_token& b)
    {
        if (a.stop_possible()) m_a = std::make_unique<std::stop_callback<Relay>>(a, Relay{ &m_source });
        if (b.stop_possible()) m_b = std::make_unique<std::stop_callback<Relay>>(b, Relay{ &m_source });
    }

    namespace
    {
        std::atomic<int> g_backgroundTasks{ 0 };
        std::mutex g_handlerMutex;
        std::function<void(const char*)> g_errorHandler;

        void ReportBackgroundError(const char* what)
        {
            std::function<void(const char*)> handler;
            {
                std::lock_guard<std::mutex> lock(g_handlerMutex);
                handler = g_errorHandler;
            }
            if (handler)
            {
                try { handler(what); } catch (...) {}
            }
            else
            {
                OutputDebugStringA("[RunDetached] ");
                OutputDebugStringA(what);
                OutputDebugStringA("\n");
            }
        }
    }

    void SetBackgroundErrorHandler(std::function<void(const char* what)> handler)
    {
        std::lock_guard<std::mutex> lock(g_handlerMutex);
        g_errorHandler = std::move(handler);
    }

    void RunDetached(std::function<void()> fn)
    {
        ++g_backgroundTasks;
        std::thread([fn = std::move(fn)]() mutable {
            try { fn(); }
            catch (const OperationCanceled&) {}
            catch (const std::exception& ex) { ReportBackgroundError(ex.what()); }
            catch (...) { ReportBackgroundError("unknown exception"); }
            fn = nullptr;   // huỷ các biến bị capture TRƯỚC khi báo đã xong
            --g_backgroundTasks;
        }).detach();
    }

    int BackgroundTaskCount() { return g_backgroundTasks.load(); }

    bool WaitBackgroundIdle(int timeoutMs)
    {
        Stopwatch sw;
        while (g_backgroundTasks.load() > 0)
        {
            if (sw.ElapsedMilliseconds() >= timeoutMs) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    bool WhenAll(std::vector<std::function<bool()>> jobs)
    {
        if (jobs.empty()) return true;

        std::vector<std::future<bool>> futures;
        futures.reserve(jobs.size() - 1);
        for (size_t i = 1; i < jobs.size(); ++i)
            futures.push_back(std::async(std::launch::async, std::move(jobs[i])));

        bool all = true;
        std::exception_ptr firstError;
        try { all = jobs[0]() && all; }
        catch (...) { firstError = std::current_exception(); }

        for (auto& f : futures)
        {
            try { all = f.get() && all; }
            catch (...) { if (!firstError) firstError = std::current_exception(); }
        }

        if (firstError) std::rethrow_exception(firstError);
        return all;
    }
}
