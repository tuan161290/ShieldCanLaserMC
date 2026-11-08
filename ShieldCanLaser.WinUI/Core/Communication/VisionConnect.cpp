#include "Communication/VisionConnect.h"

#include "Base/Async.h"
#include "Base/Text.h"
#include "Infor/AlarmCode.h"
#include "Infor/AlarmManager.h"
#include "Utility/Log/LogWorker.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <optional>

namespace scs
{
    struct VisionCmdGate
    {
        std::recursive_mutex cmdLock;
    };

    namespace
    {
        struct FactorySlot
        {
            std::mutex mutex;
            std::function<std::shared_ptr<IVisionDevice>()> factory;
        };

        FactorySlot& Slot()
        {
            static FactorySlot slot;
            return slot;
        }

        void LogError(const std::string& message)
        {
            LogWorker::Instance().RecordLog(message, LogLevel::Error, "Vision");
        }

        std::string What(const std::exception& ex)
        {
            std::string w = ex.what();
            return w.empty() ? "Unknown error" : w;
        }

        /// <summary>
        /// Lệnh Run của SDK là lệnh CHẶN, không nhận token huỷ — chạy trên luồng nền rồi canh thời gian. Hết giờ
        /// (hoặc bị huỷ) thì trả nullopt cho lớp gọi báo alarm 4003, nhưng luồng chạy Run bên dưới KHÔNG bị huỷ
        /// thật — SDK không cho — nó tự trôi tới khi solution phản hồi.
        /// </summary>
        template <class T>
        std::optional<T> RunWithTimeout(const std::shared_ptr<VisionCmdGate>& gate, int timeoutMs, std::function<T()> body, const std::stop_token& ct)
        {
            struct State
            {
                std::mutex mutex;
                std::condition_variable_any cv;
                bool done{ false };
                std::optional<T> value;
                std::exception_ptr error;
                std::atomic<int> abandoned{ 0 };
            };
            auto st = std::make_shared<State>();

            base::RunDetached([gate, st, body = std::move(body)]
            {
                std::optional<T> value;
                std::exception_ptr error;
                try
                {
                    std::lock_guard<std::recursive_mutex> lock(gate->cmdLock);
                    // Lần gọi đã bị bỏ (quá giờ/huỷ) trong lúc còn xếp hàng chờ khoá — ví dụ sau một Run treo — thì
                    // không chạy muộn nữa: chụp muộn là chụp nhầm can (trục đã đi chỗ khác).
                    if (st->abandoned.load() != 1) value = body();
                }
                catch (...) { error = std::current_exception(); }
                {
                    std::lock_guard<std::mutex> lock(st->mutex);
                    st->value = std::move(value);
                    st->error = error;
                    st->done = true;
                }
                st->cv.notify_all();
            });

            std::unique_lock<std::mutex> lock(st->mutex);
            auto finished = [&st] { return st->done; };
            bool done = timeoutMs < 0 ? st->cv.wait(lock, ct, finished)
                                      : st->cv.wait_for(lock, ct, std::chrono::milliseconds(timeoutMs), finished);
            if (done)
            {
                if (st->error) std::rethrow_exception(st->error);
                return std::move(st->value);
            }
            st->abandoned.store(1);
            return std::nullopt;
        }
    }

    VisionConnect::VisionConnect(std::shared_ptr<SystemData> sys)
        : m_sys(sys ? std::move(sys) : std::make_shared<SystemData>()), m_gate(std::make_shared<VisionCmdGate>())
    {
        // Mô phỏng mà có thiết bị VisionMaster thật (app đã đăng ký hàm tạo) thì vẫn dùng nó, trừ khi Setting chọn
        // vision giả: VisionMaster không đụng trục/IO. Không có hàm tạo (bài kiểm thử, chế độ chụp ảnh) thì bản giả.
        if (m_sys->SimulationMode && (m_sys->VisionSimulation || !HasRealDeviceFactory()))
        {
            m_fake = std::make_shared<FakeVision>();
            auto s = m_sys;
            m_fake->SetNames([s] { return s->VisionOutputs; });
        }
    }

    void VisionConnect::SetRealDeviceFactory(std::function<std::shared_ptr<IVisionDevice>()> factory)
    {
        std::lock_guard<std::mutex> lock(Slot().mutex);
        Slot().factory = std::move(factory);
    }

    bool VisionConnect::HasRealDeviceFactory()
    {
        std::lock_guard<std::mutex> lock(Slot().mutex);
        return static_cast<bool>(Slot().factory);
    }

    std::shared_ptr<IVisionDevice> VisionConnect::Device() const
    {
        if (m_fake) return m_fake;
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        return m_device;
    }

    std::shared_ptr<IVisionDevice> VisionConnect::EnsureDevice(std::string& why)
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        if (m_device) return m_device;

        std::function<std::shared_ptr<IVisionDevice>()> factory;
        {
            std::lock_guard<std::mutex> slotLock(Slot().mutex);
            factory = Slot().factory;
        }
        if (!factory)
        {
            why = "Real VisionMaster device not registered (VisionConnect::SetRealDeviceFactory)";
            return nullptr;
        }
        try { m_device = factory(); }
        catch (const std::exception& ex) { why = "Failed to create VisionMaster device — " + What(ex); return nullptr; }
        catch (...) { why = "Failed to create VisionMaster device — unknown error"; return nullptr; }
        if (!m_device) why = "VisionMaster device factory returned null";
        return m_device;
    }

    bool VisionConnect::IsConnected() const { return m_fake ? m_fake->IsConnected() : m_connected.load(); }

    bool VisionConnect::IsSolutionLoaded() const
    {
        auto dev = Device();
        return dev && dev->IsSolutionLoaded();
    }

    bool VisionConnect::Connect()
    {
        if (m_fake) return m_fake->Connect();

        std::string why;
        auto dev = EnsureDevice(why);
        if (!dev)
        {
            // Không có thiết bị thật thì KHÔNG lặng lẽ chạy bản giả: máy thật mà "thấy" bằng số bịa là khắc sai chỗ.
            LogError("Vision: no real device — " + why);
            AlarmManager::Raise(AlarmCode::VisionLoadFailed, "Vision", why);
            m_connected = false;
            return false;
        }
        bool ok = false;
        try { ok = dev->Connect(); }
        catch (const std::exception& ex)
        {
            LogError("Vision: connect failed — " + What(ex)
                     + (m_sys->SimulationMode ? " (if VisionMaster is not installed, enable \"Fake vision\" on Setting page)" : ""));
        }
        m_connected = ok;
        return ok;
    }

    void VisionConnect::Disconnect()
    {
        if (m_fake) { m_fake->Disconnect(); return; }
        auto dev = Device();
        try { if (dev) dev->Disconnect(); } catch (...) {}
        m_connected = false;
    }

    bool VisionConnect::LoadSolution(const std::string& solutionPath)
    {
        if (m_fake) return m_fake->LoadSolution(solutionPath);

        std::string why;
        auto dev = EnsureDevice(why);
        if (!dev)
        {
            LogError("Vision: load solution failed — " + why);
            AlarmManager::Raise(AlarmCode::VisionLoadFailed, "Vision", why);
            return false;
        }
        try
        {
            std::lock_guard<std::recursive_mutex> lock(m_gate->cmdLock);
            if (dev->LoadSolution(solutionPath)) return true;
            LogError("Vision: load solution failed — device could not load " + solutionPath);
            return false;
        }
        catch (const std::exception& ex)
        {
            LogError("Vision: load solution failed — " + What(ex));
            return false;
        }
    }

    std::string VisionConnect::CheckModulePath(const std::string& path)
    {
        if (m_fake) return m_fake->CheckModulePath(path);

        if (base::Trim(path).empty()) return "Path not set on Setting page";
        if (!IsConnected()) return "Not connected";
        if (!IsSolutionLoaded()) return "Solution not loaded";

        auto dev = Device();
        try
        {
            std::lock_guard<std::recursive_mutex> lock(m_gate->cmdLock);
            return dev->CheckModulePath(path);
        }
        catch (const std::exception& ex) { return What(ex); }
    }

    VmCallResult VisionConnect::CallFlow(const VmCallRequest& request, std::stop_token ct)
    {
        if (m_fake) return m_fake->CallFlow(request, ct);

        if (request.Flow.empty()) return VmCallResult::Fail("Missing Flow name");
        if (!IsConnected()) return VmCallResult::Fail("Not connected");
        if (!IsSolutionLoaded()) return VmCallResult::Fail("Solution not loaded");

        auto dev = Device();
        try
        {
            // Chạy, đọc ngay — cùng một lượt giữ khoá lệnh của thiết bị.
            auto r = RunWithTimeout<VmCallResult>(m_gate, m_sys->VisionTriggerTimeoutMs,
                [dev, request, ct] { return dev->CallFlow(request, ct); }, ct);

            if (!r)
            {
                // Huỷ (EMG, rèm, STOP huỷ chu trình) giữa lúc Flow chạy không phải quá giờ — không alarm. Quá giờ thật
                // thì phát dạng SỰ KIỆN cho RESET gỡ được.
                if (ct.stop_requested()) return VmCallResult::Fail("Canceled while running Flow " + request.Flow);
                AlarmManager::RaiseEvent(AlarmCode::VisionTriggerTimeout, "Vision", request.Flow);
                return VmCallResult::Fail("Timeout waiting for Flow " + request.Flow, true);
            }
            return *r;
        }
        catch (const base::OperationCanceled&) { return VmCallResult::Fail("Canceled while running Flow " + request.Flow); }
        catch (const std::exception& ex)
        {
            LogError("Vision: " + request.Flow + " failed — " + What(ex));
            return VmCallResult::Fail(request.Flow + ": " + What(ex));
        }
    }

    std::string VisionConnect::SaveSolution()
    {
        if (m_fake) return m_fake->SaveSolution();

        if (!IsSolutionLoaded()) return "Solution not loaded";
        auto dev = Device();
        try
        {
            std::lock_guard<std::recursive_mutex> lock(m_gate->cmdLock);
            return dev->SaveSolution();
        }
        catch (const std::exception& ex) { return What(ex); }
    }

    std::shared_ptr<const ImageData> VisionConnect::LastImage() const
    {
        auto dev = Device();
        return dev ? dev->LastImage() : nullptr;
    }

    int VisionConnect::LastImageNozzle() const
    {
        auto dev = Device();
        return dev ? dev->LastImageNozzle() : 0;
    }
}
