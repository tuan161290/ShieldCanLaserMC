#include "Vision/VmImageConvert.h"

#include <cstring>
#include <format>

namespace scs
{
    std::shared_ptr<const ImageData> VmImageToImageData(const void* data, std::size_t dataLen, int width, int height,
                                                        unsigned pixelFormat, std::string* why)
    {
        auto fail = [why](const std::string& reason) -> std::shared_ptr<const ImageData>
        {
            if (why) *why = reason;
            return nullptr;
        };

        if (data == nullptr) return fail("Empty image (null data pointer)");
        if (width <= 0 || height <= 0) return fail(std::format("Invalid image size {}x{}", width, height));

        int channels = 0;
        switch (pixelFormat)
        {
        case VmPixelFormat::Mono8: channels = 1; break;
        case VmPixelFormat::Rgb24Packed:
        case VmPixelFormat::Rgb24Planar: channels = 3; break;
        default: return fail(std::format("Unsupported pixel format 0x{:08X}", pixelFormat));
        }

        const std::size_t plane = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        const std::size_t need = plane * static_cast<std::size_t>(channels);
        if (dataLen < need)
            return fail(std::format("Image data short: got {} bytes, need {} ({}x{}x{})", dataLen, need, width, height, channels));

        auto img = std::make_shared<ImageData>();
        img->Width = width;
        img->Height = height;
        img->Channels = channels;
        img->Stride = width * channels;
        img->Pixels.resize(need);

        const auto* src = static_cast<const unsigned char*>(data);
        unsigned char* dst = img->Pixels.data();

        if (pixelFormat == VmPixelFormat::Mono8)
        {
            std::memcpy(dst, src, need);
        }
        else if (pixelFormat == VmPixelFormat::Rgb24Packed)
        {
            for (std::size_t i = 0; i < plane; ++i)
            {
                dst[i * 3 + 0] = src[i * 3 + 2];   // B
                dst[i * 3 + 1] = src[i * 3 + 1];   // G
                dst[i * 3 + 2] = src[i * 3 + 0];   // R
            }
        }
        else
        {
            const unsigned char* r = src;
            const unsigned char* g = src + plane;
            const unsigned char* b = src + plane * 2;
            for (std::size_t i = 0; i < plane; ++i)
            {
                dst[i * 3 + 0] = b[i];
                dst[i * 3 + 1] = g[i];
                dst[i * 3 + 2] = r[i];
            }
        }
        return img;
    }
}
