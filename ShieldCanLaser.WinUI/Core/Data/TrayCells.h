#pragma once
#include <mutex>
#include <vector>

namespace scs
{
    /// <summary>
    /// Sổ ô của một tray: ô đã chạy xong và ô người vận hành đang chọn để chạy lẻ. Luồng chu trình ghi, luồng UI đọc và chọn
    /// cùng lúc — mọi truy cập qua khoá. Chỉ số ô 0-based = hàng × 7 + cột (cột = nozzle − 1).
    /// </summary>
    class TrayCells
    {
    public:
        /// <summary>Số ô đã xong trong <paramref name="cells"/> ô đầu (model có thể vừa giảm số hàng).</summary>
        int DoneCount(int cells) const;
        bool IsDone(int cell) const;
        /// <summary>Ô vừa chạy xong: ghi nhận và bỏ chọn nó.</summary>
        void MarkDone(int cell);
        /// <summary>Các ô đã xong, tăng dần.</summary>
        std::vector<int> Done() const;
        /// <summary>Nạp nguyên trạng (running.json).</summary>
        void LoadDone(std::vector<int> cells);
        void ClearDone();

        // ── Chọn ô chạy lẻ ──
        /// <summary>Các ô đang chọn, tăng dần.</summary>
        std::vector<int> Selected() const;
        bool HasSelection() const;
        bool IsSelected(int cell) const;
        void Toggle(int cell);
        /// <summary>Chọn / bỏ chọn nhiều ô một lần (kéo chọn vùng).</summary>
        void SetSelected(const std::vector<int>& cells, bool on);
        void ClearSelection();

    private:
        mutable std::mutex m_mutex;
        std::vector<int> m_done;       // tăng dần, không trùng
        std::vector<int> m_selected;   // tăng dần, không trùng
    };
}
