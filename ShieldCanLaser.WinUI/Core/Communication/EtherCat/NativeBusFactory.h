#pragma once
#include <memory>
#include <string>

#include "EtherCat/Transport/IEtherCatBus.h"

namespace scs
{
    /// <summary>
    /// Tạo bus SOEM thật (<c>ecm::SoemEtherCatBus</c>). Bản C# nạp một DLL C++/CLI qua reflection;
    /// ở đây bus thật là lớp native biên dịch thẳng vào thư viện này, nên điều kiện duy nhất còn lại
    /// là Npcap (wpcap.dll/Packet.dll) phải có trên máy. Giữ đúng quy tắc của Partron CAM TEST: KHÔNG
    /// âm thầm rơi về mô phỏng khi đã chọn phần cứng thật mà bus thật lại không dựng được — trên màn
    /// hình hai thứ đó trông giống hệt nhau.
    /// </summary>
    class NativeBusFactory
    {
    public:
        /// <summary>Bus thật dùng được trên máy này không (= Npcap nạp được).</summary>
        static bool IsAvailable();

        /// <summary>Tạo bus SOEM. Thất bại (bus = nullptr, error = lý do) khi thiếu Npcap — không bao giờ
        /// trả về bus mô phỏng thay thế.</summary>
        static bool TryCreateSoemBus(std::shared_ptr<ecm::IEtherCatBus>& bus, std::string& error);
    };
}
