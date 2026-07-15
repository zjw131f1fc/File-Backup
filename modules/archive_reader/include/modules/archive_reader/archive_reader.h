#pragma once

#include "common/result.h"
#include "common/entry_info.h"
#include <string>
#include <istream>
#include <memory>

namespace backup {

class IArchiveReader {
public:
    virtual ~IArchiveReader() = default;

    // 检查格式标识、版本、截断、非法长度和危险路径
    virtual Result validate() = 0;

    // 是否还有下一个条目
    virtual bool has_next_entry() = 0;

    // 读取下一个条目信息
    virtual Result next_entry(EntryInfo& entry_info) = 0;

    // 流式打开普通文件内容（非普通文件返回 nullptr）
    virtual std::unique_ptr<std::istream> open_content(const EntryInfo& entry_info) = 0;
};

// 工厂函数：打开归档
std::unique_ptr<IArchiveReader> open_archive(const std::string& archive_path);

}  // namespace backup
