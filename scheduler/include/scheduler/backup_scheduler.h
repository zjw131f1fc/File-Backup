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
        IScanner& scanner,
        IFilter& filter,
        IArchiveWriter& archive_writer
    );

    // 执行备份任务
    Result run(const std::string& task_id, const BackupRequest& request);

private:
    void update_progress(const std::string& task_id,
                         const std::string& stage,
                         const std::string& current_path);
    Result scan_source(const std::string& task_id, const BackupRequest& request);
    Result finish_archive(const std::string& task_id,
                          const BackupRequest& request,
                          const Result& scan_result);

    TaskManager& task_manager_;
    IScanner* scanner_;
    IFilter* filter_;
    IArchiveWriter* archive_writer_;
};

}  // namespace backup
