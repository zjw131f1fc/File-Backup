#pragma once

#include "common/entry_type.h"
#include <string>
#include <vector>
#include <cstdint>
#include <sys/types.h>

namespace backup {

// 六类筛选条件
struct FilterRules {
    // 路径筛选：包含/排除的路径模式（glob）
    std::vector<std::string> include_paths;
    std::vector<std::string> exclude_paths;

    // 类型筛选：只包含的文件类型（空 = 全部）
    std::vector<EntryType> include_types;

    // 名称筛选：glob 模式
    std::vector<std::string> include_names;
    std::vector<std::string> exclude_names;

    // 时间筛选（Unix 时间戳秒，0 = 不限制）
    int64_t newer_than_sec = 0;
    int64_t older_than_sec = 0;

    // 尺寸筛选（字节，0 = 不限制）
    uint64_t min_size = 0;
    uint64_t max_size = 0;

    // 用户筛选：UID 列表（空 = 全部）
    std::vector<uid_t> include_uids;
};

}  // namespace backup
