#pragma once

#include "common/result.h"
#include "common/entry_info.h"
#include "common/conflict_policy.h"
#include "modules/archive_reader/archive_reader.h"
#include <string>
#include <memory>

namespace backup {

class IRestorer {
public:
    virtual ~IRestorer() = default;

    // 恢复一个条目到目标目录
    // 恢复器内部根据 entry_info.type 决定行为：
    //   REGULAR_FILE → 从 reader 取流，写临时文件后替换
    //   DIRECTORY → mkdir
    //   SYMBOLIC_LINK → symlink
    //   HARD_LINK → link
    //   FIFO → mkfifo
    //   CHARACTER_DEVICE / BLOCK_DEVICE → mknod
    // 调度器不需要判断类型，只调这一个接口
    virtual Result restore_entry(
        const std::string& target_root,
        const EntryInfo& entry_info,
        IArchiveReader& reader,
        ConflictPolicy conflict_policy
    ) = 0;

    // 恢复元数据（权限、属主、时间）
    // 目录的时间戳需要在子文件创建后恢复，因此单独提供
    virtual Result restore_metadata(
        const std::string& target_path,
        const EntryInfo& entry_info
    ) = 0;
};

// 工厂函数：创建恢复器实例
std::unique_ptr<IRestorer> create_restorer();

}  // namespace backup
