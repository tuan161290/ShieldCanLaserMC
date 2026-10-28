#pragma once
// Thay cho `event Action<...>` của C#. Handler được gọi trên luồng phát sự kiện — bên nhận tự
// chuyển về luồng UI nếu cần (xem scs::UiThread).
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace base
{
    template <class... Args>
    class Event
    {
    public:
        using Handler = std::function<void(Args...)>;

        /// <summary>Đăng ký; trả token để Unsubscribe.</summary>
        int Subscribe(Handler handler)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            int token = ++m_nextToken;
            m_handlers.emplace_back(token, std::move(handler));
            return token;
        }

        void Unsubscribe(int token)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_handlers.begin(); it != m_handlers.end(); ++it)
            {
                if (it->first == token) { m_handlers.erase(it); return; }
            }
        }

        int operator+=(Handler handler) { return Subscribe(std::move(handler)); }

        /// <summary>Gọi mọi handler. Chụp danh sách dưới khoá rồi gọi NGOÀI khoá, nên handler được
        /// phép đăng ký/huỷ đăng ký hoặc phát lại chính sự kiện này.</summary>
        void Raise(Args... args) const
        {
            std::vector<std::pair<int, Handler>> snapshot;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                snapshot = m_handlers;
            }
            for (auto& h : snapshot)
            {
                if (h.second) h.second(args...);
            }
        }

        void operator()(Args... args) const { Raise(args...); }

        bool Empty() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_handlers.empty();
        }

    private:
        mutable std::mutex m_mutex;
        std::vector<std::pair<int, Handler>> m_handlers;
        int m_nextToken{ 0 };
    };
}
