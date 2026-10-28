#pragma once
#include <cstdint>
#include <vector>

namespace scs
{
    /// <summary>
    /// Ảnh thô thay cho <c>System.Drawing.Bitmap</c> của bản C#. Truyền đi dưới dạng
    /// <c>std::shared_ptr&lt;const ImageData&gt;</c> (nullptr = không có ảnh) nên luồng UI đọc được ảnh
    /// trong lúc luồng vision đã thay ảnh mới. Dòng đầu là dòng TRÊN cùng; mỗi dòng dài
    /// <see cref="Stride"/> byte (≥ Width × Channels).
    /// </summary>
    struct ImageData
    {
        int Width{};
        int Height{};
        int Channels{};   // 1 = Gray8, 3 = BGR24, 4 = BGRA32
        int Stride{};
        std::vector<uint8_t> Pixels;
    };
}
