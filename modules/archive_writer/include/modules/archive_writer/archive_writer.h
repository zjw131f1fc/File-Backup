#pragma once

#include "common/result.h"
#include "common/entry_info.h"
#include <string>
#include <istream>
#include <memory>

namespace backup {

class IArchiveWriter {
public:
    virtual ~IArchiveWriter() = default;

    // 写入有内容的条目（普通文件）
    virtual Result add_entry(
        const EntryInfo& entry_info,
        std::istream& content
    ) = 0;

    // 写入无内容的条目（目录、链接、特殊文件等）
    virtual Result add_entry(const EntryInfo& entry_info) = 0;

    // 成功时提交归档
    virtual Result commit() = 0;

    // 失败或取消时删除临时归档
    virtual Result abort() = 0;
};

// 工厂函数：创建归档写入器
std::unique_ptr<IArchiveWriter> create_archive(const std::string& output_path);

}  // namespace backup
