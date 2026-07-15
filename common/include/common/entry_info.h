#pragma once

#include "common/entry_type.h"
#include <string>
#include <cstdint>
#include <sys/types.h>

namespace backup {

struct EntryInfo {
    std::string path;              // 归档内的相对路径
    EntryType type = EntryType::REGULAR_FILE;
    uint64_t size = 0;             // 普通文件大小
    std::string link_target;       // 符号链接目标
    std::string hard_link_target;  // 硬链接指向的首次路径
    uint64_t hard_link_inode = 0;  // 硬链接 inode（首次出现的文件为 0）
    mode_t permissions = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    int64_t atime_sec = 0;
    int64_t atime_nsec = 0;
    int64_t mtime_sec = 0;
    int64_t mtime_nsec = 0;
    uint32_t device_major = 0;     // 字符/块设备主号
    uint32_t device_minor = 0;     // 字符/块设备次号
};

}  // namespace backup
