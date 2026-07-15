#pragma once

namespace backup {

enum class ConflictPolicy {
    SKIP,       // 跳过已存在的文件
    OVERWRITE,  // 覆盖已存在的文件
    RENAME,     // 重命名新文件
};

}  // namespace backup
