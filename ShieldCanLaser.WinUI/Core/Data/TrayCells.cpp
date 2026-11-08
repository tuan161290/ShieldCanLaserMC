#include "Data/TrayCells.h"

#include <algorithm>

namespace scs
{
    int TrayCells::DoneCount(int cells) const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        return (int)(std::lower_bound(m_done.begin(), m_done.end(), cells) - m_done.begin());
    }

    bool TrayCells::IsDone(int cell) const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        return std::binary_search(m_done.begin(), m_done.end(), cell);
    }

    void TrayCells::MarkDone(int cell)
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (cell < 0) return;
        auto it = std::lower_bound(m_done.begin(), m_done.end(), cell);
        if (it == m_done.end() || *it != cell) m_done.insert(it, cell);
        std::erase(m_selected, cell);
    }

    std::vector<int> TrayCells::Done() const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        return m_done;
    }

    void TrayCells::LoadDone(std::vector<int> cells)
    {
        std::lock_guard<std::mutex> l(m_mutex);
        std::erase_if(cells, [](int c) { return c < 0; });
        std::sort(cells.begin(), cells.end());
        cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
        m_done = std::move(cells);
    }

    void TrayCells::ClearDone()
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_done.clear();
    }

    std::vector<int> TrayCells::Selected() const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        return m_selected;
    }

    bool TrayCells::HasSelection() const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        return !m_selected.empty();
    }

    bool TrayCells::IsSelected(int cell) const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        return std::binary_search(m_selected.begin(), m_selected.end(), cell);
    }

    void TrayCells::Toggle(int cell)
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (cell < 0) return;
        auto it = std::lower_bound(m_selected.begin(), m_selected.end(), cell);
        if (it != m_selected.end() && *it == cell) m_selected.erase(it);
        else m_selected.insert(it, cell);
    }

    void TrayCells::SetSelected(const std::vector<int>& cells, bool on)
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (int cell : cells)
        {
            if (cell < 0) continue;
            auto it = std::lower_bound(m_selected.begin(), m_selected.end(), cell);
            const bool has = it != m_selected.end() && *it == cell;
            if (on && !has) m_selected.insert(it, cell);
            else if (!on && has) m_selected.erase(it);
        }
    }

    void TrayCells::ClearSelection()
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_selected.clear();
    }
}
