#pragma once
#include <cstdint>
#include <format>
#include <string>

namespace scs
{
    /// <summary>
    /// Cấu hình Remote IO EtherCAT — Leadshine R1EC + module 32 DI + module 32 DO (theo thứ tự đó trong rack). Module
    /// trong rack được bộ ghép tự báo lúc kết nối; ở đây chỉ định danh bộ ghép.
    /// Tách khỏi kiểu của thư viện EtherCAT (ecm) để tầng Controller không cần biết kiểu đó.
    /// </summary>
    struct IoModuleConfig
    {
        std::string Name = "R1EC";

        /// <summary>Vị trí trên chuỗi EtherCAT — mặc định ngay sau 3 driver X/Y/Z (slave 3).</summary>
        int SlavePosition = 9;

        uint32_t ExpectedVendorId = 0;
        uint32_t ExpectedProductCode = 0;
        uint32_t ExpectedRevision = 0;
        std::string ExpectedDriverName = "R1EC";

        // Hai hàm dưới không ghi vào JSON ([JsonIgnore]).
        bool IsConfigured() const { return ExpectedProductCode != 0; }

        /// <summary>"Vendor/Product" dạng hex — chỉ để hiển thị ở trang Setting.</summary>
        std::string DriverIdText() const
        {
            return IsConfigured() ? std::format("0x{:X}/0x{:X}", ExpectedVendorId, ExpectedProductCode) : "(unassigned)";
        }
    };
}
