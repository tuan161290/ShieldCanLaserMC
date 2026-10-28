#pragma once
// Cách DUY NHẤT để include SOEM từ mã C++.
//
// pcap-stdinc.h (soem.h kéo theo trên Windows) có `#define inline __inline` cho C89. Thư viện chuẩn
// C++ báo lỗi cứng nếu `inline` còn là macro lúc nó được xử lý (xkeycheck.h), nên phải gỡ macro đó
// ngay sau khi include SOEM, và mọi header chuẩn C++ cần dùng phải được include TRƯỚC file này hoặc
// sau khi đã gỡ.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

// osal_defs.h của SOEM tự định nghĩa lại WIN32_LEAN_AND_MEAN (C4005) — vô hại.
#pragma warning(push)
#pragma warning(disable : 4005)
extern "C" {
#include "soem/soem.h"
}
#pragma warning(pop)

#ifdef inline
#undef inline
#endif
