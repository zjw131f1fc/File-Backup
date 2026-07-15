#pragma once

#include "common/result.h"
#include "common/progress.h"
#include "common/entry_info.h"
#include "modules/filter/filter.h"
#include "modules/archive_writer/archive_writer.h"
#include <string>
#include <functional>
#include <memory>

namespace backup {

using ProgressCallback = std::function<bool(const Progress&)>;

class IScanner {
public:
    virtual ~IScanner() = default;

    // 扫描源目录，内部：扫描 → 筛选 → 开源文件流 → 写入归档
    virtual Result scan_and_backup(
        const std::string& source_path,
        IFilter& filter,
        IArchiveWriter& archive_writer,
        ProgressCallback progress_callback
    ) = 0;
};

// 工厂函数：创建扫描器实例
std::unique_ptr<IScanner> create_scanner();

}  // namespace backup
