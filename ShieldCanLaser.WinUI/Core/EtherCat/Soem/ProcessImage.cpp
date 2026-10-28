#include "EtherCat/Soem/ProcessImage.h"

#include <chrono>
#include <cstring>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#define ECM_CPU_RELAX() _mm_pause()
#else
#define ECM_CPU_RELAX() ((void)0)
#endif

namespace ecm::rt
{
    void ProcessImage::Attach(uint8_t* outputs, size_t outputBytes, const uint8_t* inputs, size_t inputBytes)
    {
        std::lock_guard<std::mutex> lock(m_writerMutex);
        m_outputs = outputs;
        m_outputBytes = outputs != nullptr ? outputBytes : 0;
        m_inputs = inputs;
        m_inputBytes = inputs != nullptr ? inputBytes : 0;

        m_outputShadow.assign(m_outputBytes, 0);
        if (m_outputBytes > 0) std::memcpy(m_outputShadow.data(), m_outputs, m_outputBytes);

        m_inputSnapshot.assign(m_inputBytes, 0);
        if (m_inputBytes > 0) std::memcpy(m_inputSnapshot.data(), m_inputs, m_inputBytes);

        m_head.store(0, std::memory_order_release);
        m_tail.store(0, std::memory_order_release);
        m_inputSeq.store(0, std::memory_order_release);
    }

    void ProcessImage::Detach()
    {
        std::lock_guard<std::mutex> lock(m_writerMutex);
        m_outputs = nullptr;
        m_inputs = nullptr;
        m_outputBytes = 0;
        m_inputBytes = 0;
        m_head.store(0, std::memory_order_release);
        m_tail.store(0, std::memory_order_release);
    }

    void ProcessImage::WriteOutput(size_t offset, const void* data, size_t length)
    {
        if (length == 0 || length > sizeof(Write::Data)) return;

        std::lock_guard<std::mutex> lock(m_writerMutex);
        if (m_outputs == nullptr || offset + length > m_outputBytes) return;

        std::memcpy(m_outputShadow.data() + offset, data, length);

        // Hàng đợi đầy nghĩa là luồng chu kỳ chưa rút kịp (1024 lệnh trong một chu kỳ) — chờ nó rút.
        // Luồng chu kỳ không cần m_writerMutex nên chờ ở đây không khoá chết.
        uint32_t tail = m_tail.load(std::memory_order_relaxed);
        for (int spins = 0; tail - m_head.load(std::memory_order_acquire) >= QueueSize; ++spins)
        {
            if (spins > 2000) return;   // luồng chu kỳ đã dừng: bản shadow vẫn giữ giá trị, bỏ lệnh ghi
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        Write& w = m_queue[tail & QueueMask];
        w.Offset = (uint16_t)offset;
        w.Length = (uint8_t)length;
        std::memcpy(w.Data, data, length);
        m_tail.store(tail + 1, std::memory_order_release);   // công bố: từ đây luồng chu kỳ mới thấy
    }

    void ProcessImage::ReadOutputShadow(size_t offset, void* data, size_t length) const
    {
        std::lock_guard<std::mutex> lock(m_writerMutex);
        if (offset + length > m_outputShadow.size()) { std::memset(data, 0, length); return; }
        std::memcpy(data, m_outputShadow.data() + offset, length);
    }

    void ProcessImage::ReadInput(size_t offset, void* data, size_t length) const
    {
        if (offset + length > m_inputSnapshot.size()) { std::memset(data, 0, length); return; }

        const uint8_t* src = m_inputSnapshot.data() + offset;
        for (;;)
        {
            uint32_t s1 = m_inputSeq.load(std::memory_order_acquire);
            if ((s1 & 1u) == 0)
            {
                std::memcpy(data, src, length);
                if (m_inputSeq.load(std::memory_order_acquire) == s1) return;
            }
            ECM_CPU_RELAX();
        }
    }

    void ProcessImage::ApplyPendingOutputs()
    {
        if (m_outputs == nullptr) return;

        uint32_t head = m_head.load(std::memory_order_relaxed);
        const uint32_t tail = m_tail.load(std::memory_order_acquire);
        while (head != tail)
        {
            const Write& w = m_queue[head & QueueMask];
            if ((size_t)w.Offset + w.Length <= m_outputBytes)
                std::memcpy(m_outputs + w.Offset, w.Data, w.Length);
            ++head;
        }
        m_head.store(head, std::memory_order_release);
    }

    void ProcessImage::PublishInputs()
    {
        if (m_inputs == nullptr || m_inputBytes == 0) return;

        m_inputSeq.fetch_add(1, std::memory_order_acq_rel);   // lẻ: đang ghi
        std::memcpy(m_inputSnapshot.data(), m_inputs, m_inputBytes);
        m_inputSeq.fetch_add(1, std::memory_order_release);   // chẵn: xong
    }
}
