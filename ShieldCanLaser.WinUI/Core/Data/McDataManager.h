#pragma once
#include "Data/Model/ModelData.h"
#include "Data/RunningData.h"
#include "Data/SystemData.h"

#include <memory>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>Nạp và lưu toàn bộ dữ liệu bền vững: system.json, model, running.json. Ba đối tượng
    /// hiện hành được chia sẻ qua shared_ptr (như tham chiếu C#) và không bao giờ null.</summary>
    class McDataManager
    {
    public:
        static std::shared_ptr<SystemData> SysData();
        static std::shared_ptr<RunningData> RunData();
        static std::shared_ptr<ModelData> Model();
        static bool ModelLoaded();

        static void LoadSystemData();
        static bool SaveSystemData();

        static void LoadRunningData();
        /// <summary>Ghi tiến độ — phục vụ log/điều tra và giữ bộ đếm OK/NG, KHÔNG dùng để resume chu
        /// trình: LoadRunningData luôn xoá tiến độ đọc được ngay sau khi nạp.</summary>
        static bool SaveRunningData();

        static std::vector<std::string> ListModels();
        static bool LoadModel(const std::string& name);
        /// <summary>Dùng một model dựng sẵn trong bộ nhớ, không đọc từ file.</summary>
        static void UseModel(std::shared_ptr<ModelData> model, bool countsAsLoaded = false);
        /// <summary>name rỗng = dùng Model()->Name.</summary>
        static bool SaveModel(const std::string& name = {});
        /// <summary>Xoá model khỏi đĩa. Không đụng tới Model đang nạp trong bộ nhớ dù tên trùng.</summary>
        static bool DeleteModel(const std::string& name);
        /// <summary>Sao chép model trên đĩa sang tên mới — đọc thẳng từ file nguồn.</summary>
        static bool CopyModel(const std::string& sourceName, const std::string& newName);
        /// <summary>Nạp lại model dùng gần nhất; nếu chưa có model nào thì tạo bản mặc định.</summary>
        static void LoadRecentModel();
    };
}
