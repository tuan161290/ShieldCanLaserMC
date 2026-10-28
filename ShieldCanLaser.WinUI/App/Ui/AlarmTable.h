#pragma once
// Bảng alarm dùng chung cho trang Alarm (đang tác động + lịch sử) và bảng chi tiết nổi của MainWindow —
// thay hai DataGrid của bản WPF (WinUI 3 không có DataGrid). Bấm vào một dòng để mở/đóng phần
// "Nguyên nhân / Xử lý" của dòng đó.
#include <set>
#include <vector>

#include "Infor/AlarmData.h"

namespace ui
{
    enum class AlarmTableKind
    {
        Active,    // Giờ | Mã | Mức | Nguồn | Gốc | Còn nguyên nhân | Nội dung | Chi tiết
        History    // Lúc phát | Lúc hết | Mã | Mức | Nguồn | Gốc | Nội dung | Chi tiết
    };

    class AlarmTable
    {
    public:
        /// <summary>Dựng khung bảng (dòng tiêu đề + vùng cuộn). Đặt phần tử trả về vào card.</summary>
        winrt::Microsoft::UI::Xaml::FrameworkElement Build(AlarmTableKind kind);

        /// <summary>Dựng lại các dòng. Chỉ gọi khi danh sách thật sự đổi (so AlarmManager::Version()).</summary>
        void Update(const std::vector<scs::AlarmData>& alarms);

    private:
        struct Column { const wchar_t* Header; double Width; };   // Width 0 = co giãn
        std::vector<Column> Columns() const;
        winrt::Microsoft::UI::Xaml::Controls::Grid MakeRowGrid() const;

        AlarmTableKind m_kind{ AlarmTableKind::Active };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_rows{ nullptr };
        /// <summary>Các dòng đang mở phần chi tiết, nhận diện bằng (mã, lúc phát) để giữ được qua lần dựng lại.</summary>
        std::set<std::pair<int, long long>> m_expanded;
    };
}
