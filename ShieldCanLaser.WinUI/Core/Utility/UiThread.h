#pragma once
#include <functional>

namespace scs
{
    /// <summary>
    /// Marshal lời gọi về UI thread. Các tầng nền (motion, I/O, routine) chạy trên luồng nền; phần
    /// nào cần chạm vào đối tượng của UI thì đi qua <see cref="Run"/>/<see cref="Post"/>.
    ///
    /// Core không phụ thuộc framework UI nào: tầng App gắn hàm "post về UI thread" bằng
    /// <see cref="SetDispatcher"/> (gọi sớm, TRÊN UI thread). Chưa gắn thì Run()/Post() chạy thẳng
    /// trên luồng gọi — đó là trường hợp console/test.
    /// </summary>
    class UiThread
    {
    public:
        /// <summary>Gắn hàm đẩy việc về UI thread và ghi nhận luồng đang gọi là UI thread.
        /// Truyền hàm rỗng để gỡ.</summary>
        static void SetDispatcher(std::function<void(std::function<void()>)> post);

        /// <summary>Ghi nhận luồng đang gọi là UI thread (vai trò của Init() bản C# — chụp
        /// SynchronizationContext hiện tại).</summary>
        static void Init();

        /// <summary>true nếu luồng đang gọi là UI thread đã ghi nhận.</summary>
        static bool IsUiThread();

        /// <summary>Chạy ngay nếu đã ở UI thread (hoặc chưa gắn dispatcher), ngược lại Post về UI
        /// thread (không chờ).</summary>
        static void Run(std::function<void()> action);

        /// <summary>Luôn đẩy qua dispatcher (không chờ); chạy thẳng khi chưa gắn dispatcher.</summary>
        static void Post(std::function<void()> fn);
    };
}
