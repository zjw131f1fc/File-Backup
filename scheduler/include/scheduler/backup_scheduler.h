#pragma once

#include "common/result.h"
#include "common/backup_request.h"
#include "common/progress.h"
#include "scheduler/task_manager.h"
#include "modules/scanner/scanner.h"
#include "modules/filter/filter.h"
#include "modules/archive_writer/archive_writer.h"
#include <memory>

namespace backup {

class BackupScheduler {
public:
    explicit BackupScheduler(
        TaskManager& task_manager,
        IScanner* scanner = nullptr,
        IArchiveWriter* archive_writer = nullptr
    );

    // 执行备份任务
    Result run(const std::string& task_id, const BackupRequest& request);

private:
    TaskManager& task_manager_;
    std::unique_ptr<IScanner> default_scanner_;
    IScanner* scanner_;
    IArchiveWriter* archive_writer_;
};

}  // namespace backup
