#pragma once
#include "Communication/ImageData.h"

#include <cstddef>
#include <memory>
#include <string>

namespace scs
{
    /// <summary>Mã định dạng điểm ảnh của SDK VisionMaster (<c>IMVS_PF_IMG_FORMAT_*</c> trong
    /// <c>iMVS-6000PlatformSDKDefine.h</c>, cũng là giá trị của <c>MvdPixelFormat</c> trong
    /// <c>VmModuleResultBase.h</c>). Chép số sang đây để phép đổi ảnh thử được mà không cần SDK.</summary>
    struct VmPixelFormat
    {
        static constexpr unsigned Mono8 = 0x01080001;        // 1 byte/điểm
        static constexpr unsigned Rgb24Packed = 0x02180014;  // R,G,B xen kẽ (RGB24 C3)
        static constexpr unsigned Rgb24Planar = 0x02180021;  // ba mặt phẳng R, G, B nối tiếp (RGB24 P3)
    };

    /// <summary>
    /// Đổi ảnh của SDK (<c>ImageBaseData</c>: con trỏ + độ dài + rộng/cao + định dạng) sang
    /// <see cref="ImageData"/> — thay cho <c>ImageBaseData_V2.ToBitmap()</c> của SDK .NET. LUÔN chép điểm ảnh:
    /// vùng nhớ của SDK chỉ sống tới lần chạy Flow kế tiếp.
    ///
    /// <para>Hỗ trợ đúng ba định dạng mà header C++ của SDK khai: Mono8 → Gray8 (Channels = 1); RGB24 xen kẽ
    /// và RGB24 ba mặt phẳng → BGR24 (Channels = 3, đảo R↔B vì <see cref="ImageData"/> quy ước BGR). SDK C++
    /// không khai định dạng BGR nào nên không có nhánh BGR đầu vào. Dòng ảnh của SDK không đệm
    /// (bước dòng = Width × số kênh — mẫu <c>GetResultControlDlg.cpp</c> đặt <c>nRowStep = Width</c>).</para>
    ///
    /// <para>Trả nullptr (kèm lý do ở <paramref name="why"/> nếu có) khi: con trỏ rỗng, kích thước ≤ 0,
    /// định dạng khác ba loại trên, hoặc <paramref name="dataLen"/> ngắn hơn số byte cần đọc.</para>
    /// </summary>
    std::shared_ptr<const ImageData> VmImageToImageData(const void* data, std::size_t dataLen, int width, int height,
                                                        unsigned pixelFormat, std::string* why = nullptr);
}
