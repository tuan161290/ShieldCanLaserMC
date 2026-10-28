#pragma once
#include <cstdint>
#include <format>
#include <string>

namespace ecm
{
    /// <summary>
    /// Địa chỉ một trục trên bus: vị trí slave + kênh trục trong slave đó, gói vào MỘT int để mọi API trục của
    /// IEtherCatBus / EtherCatMasterService (tham số tên <c>slavePosition</c>) nhận được cả driver nhiều trục mà không
    /// đổi chữ ký. <b>Kênh 0 cho ra đúng số vị trí slave</b>, nên driver một trục (Leadshine…) và module I/O dùng y như
    /// cũ.
    ///
    /// <para>Driver nhiều trục theo CiA402 (ETG.6010) đặt trục n ở dải profile <c>0x6000 + n × 0x800</c>: Welcon WE2x
    /// có trục 2 ở 0x6840 (Controlword), 0x687A (Target Position)… Bus tự dời các chỉ số 0x6000–0x67FF theo kênh
    /// (<see cref="ProfileIndex"/>); đối tượng riêng của hãng (0x2xxx) không có quy tắc chung nên bên gọi tự tính.</para>
    /// </summary>
    struct AxisAddress
    {
        /// <summary>Số kênh tối đa một slave (n = 0…7 theo ETG.6010).</summary>
        static constexpr int MaxChannels = 8;
        static constexpr uint16_t ChannelStride = 0x800;

        static constexpr int Make(int slavePosition, int channel) { return slavePosition + (channel << 16); }
        static constexpr int Slave(int address) { return address & 0xFFFF; }
        static constexpr int Channel(int address) { return (address >> 16) & 0x7; }

        /// <summary>Chỉ số đối tượng của trục: dải profile CiA402 0x6000–0x67FF dời theo kênh; chỉ số khác giữ
        /// nguyên (0x1xxx giao tiếp chung của slave, 0x2xxx của hãng).</summary>
        static constexpr uint16_t ProfileIndex(int address, uint16_t index)
        {
            return (index >= 0x6000 && index < 0x6800) ? static_cast<uint16_t>(index + Channel(address) * ChannelStride) : index;
        }

        /// <summary>"4" cho kênh 0, "4.1" cho kênh 1 — dùng trong log và thông báo.</summary>
        static std::string Text(int address)
        {
            int ch = Channel(address);
            return ch == 0 ? std::to_string(Slave(address)) : std::format("{}.{}", Slave(address), ch);
        }
    };
}
