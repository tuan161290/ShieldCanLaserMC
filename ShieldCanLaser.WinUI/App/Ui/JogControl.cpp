#include "pch.h"
#include "Ui/JogControl.h"

#include <condition_variable>
#include <deque>
#include <set>
#include <thread>

#include "Controller/McControl.h"
#include "Utility/Log/LogWorker.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Input;

namespace ui
{
    namespace
    {
        /// <summary>Một luồng nền chạy lệnh theo đúng thứ tự gửi. Luồng sống tới hết tiến trình (lệnh jog có
        /// thể còn được gửi trong lúc tắt app); hàng đợi rỗng thì ngủ.</summary>
        class SerialWorker
        {
        public:
            void Post(std::function<void()> fn)
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_queue.push_back(std::move(fn));
                    if (!m_started)
                    {
                        m_started = true;
                        std::thread([this] { Run(); }).detach();
                    }
                }
                m_cv.notify_one();
            }

        private:
            void Run()
            {
                SetThreadDescription(GetCurrentThread(), L"UI motion commands");
                for (;;)
                {
                    std::function<void()> fn;
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_cv.wait(lock, [this] { return !m_queue.empty(); });
                        fn = std::move(m_queue.front());
                        m_queue.pop_front();
                    }
                    try { fn(); }
                    catch (const std::exception& ex)
                    {
                        scs::LogWorker::Instance().RecordLog(std::string("Manual command failed: ") + ex.what(), scs::LogLevel::Error, "UI");
                    }
                    catch (...) {}
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::deque<std::function<void()>> m_queue;
            bool m_started{ false };
        };

        SerialWorker& Worker()
        {
            static SerialWorker* worker = new SerialWorker();   // cố ý không huỷ: luồng của nó chạy tới hết tiến trình
            return *worker;
        }

        struct JogState
        {
            std::string Axis;
            int Dir{};
            bool Active{ false };
        };

        // Các nút đang được giữ (chỉ chạm từ luồng UI).
        std::set<std::shared_ptr<JogState>>& ActiveJogs()
        {
            static std::set<std::shared_ptr<JogState>> active;
            return active;
        }

        void SendJog(const std::string& axis, int dir)
        {
            PostMotionCommand([axis, dir] {
                if (auto func = scs::McControl::Func()) func->Jog(axis, dir);
            });
        }

        void Release(const std::shared_ptr<JogState>& state)
        {
            if (!state->Active) return;
            state->Active = false;
            ActiveJogs().erase(state);
            SendJog(state->Axis, 0);
        }
    }

    void PostMotionCommand(std::function<void()> fn)
    {
        if (fn) Worker().Post(std::move(fn));
    }

    void AttachJog(Button const& button, std::string axisName, int dir)
    {
        auto state = std::make_shared<JogState>();
        state->Axis = std::move(axisName);
        state->Dir = dir;

        auto pressed = PointerEventHandler([state](winrt::Windows::Foundation::IInspectable const& sender, PointerRoutedEventArgs const& e) {
            if (state->Active) return;
            auto element = sender.as<UIElement>();
            element.CapturePointer(e.Pointer());   // nhả chuột ở ngoài nút vẫn nhận được PointerReleased
            state->Active = true;
            ActiveJogs().insert(state);
            SendJog(state->Axis, state->Dir);
        });
        auto released = PointerEventHandler([state](winrt::Windows::Foundation::IInspectable const&, PointerRoutedEventArgs const&) { Release(state); });

        button.AddHandler(UIElement::PointerPressedEvent(), box_value(pressed), true);
        button.AddHandler(UIElement::PointerReleasedEvent(), box_value(released), true);
        button.AddHandler(UIElement::PointerCanceledEvent(), box_value(released), true);
        button.AddHandler(UIElement::PointerCaptureLostEvent(), box_value(released), true);
        // Nút bị vô hiệu (máy chuyển bận) hoặc rời cây hiển thị giữa lúc đang giữ: dừng luôn.
        button.IsEnabledChanged([state](auto&&, auto&&) { Release(state); });
        button.Unloaded([state](auto&&, auto&&) { Release(state); });
    }

    void StopActiveJogs()
    {
        auto active = ActiveJogs();   // bản sao: Release xoá khỏi tập gốc
        for (const auto& state : active) Release(state);
    }
}
