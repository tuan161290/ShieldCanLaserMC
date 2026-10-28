#include "Utility/UiThread.h"

#include <mutex>
#include <thread>

namespace scs
{
    namespace
    {
        struct State
        {
            std::mutex mutex;
            std::function<void(std::function<void()>)> post;
            std::thread::id uiThread{};
        };

        State& S()
        {
            static State s;
            return s;
        }
    }

    void UiThread::SetDispatcher(std::function<void(std::function<void()>)> post)
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.post = std::move(post);
        s.uiThread = s.post ? std::this_thread::get_id() : std::thread::id{};
    }

    void UiThread::Init()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.uiThread = std::this_thread::get_id();
    }

    bool UiThread::IsUiThread()
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.uiThread == std::this_thread::get_id();
    }

    void UiThread::Run(std::function<void()> action)
    {
        if (!action) return;

        std::function<void(std::function<void()>)> post;
        {
            auto& s = S();
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.post && s.uiThread != std::this_thread::get_id()) post = s.post;
        }
        // Gọi NGOÀI khoá: action có thể gọi lại UiThread.
        if (post) post(std::move(action));
        else action();
    }

    void UiThread::Post(std::function<void()> fn)
    {
        if (!fn) return;

        std::function<void(std::function<void()>)> post;
        {
            auto& s = S();
            std::lock_guard<std::mutex> lock(s.mutex);
            post = s.post;
        }
        if (post) post(std::move(fn));
        else fn();
    }
}
