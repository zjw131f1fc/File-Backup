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

    // 执行一次备份：扫描源目录、筛选条目、写入归档并提交归档。
    Result run(const std::string& task_id, const BackupRequest& request);

private:
    // 向 TaskManager 写入一个阶段性的进度快照。
    void update_progress(const std::string& task_id,
                         const std::string& stage,
                         const std::string& current_path);
    // 调用 Scanner 完成扫描、筛选和归档写入，并转发进度回调。
    Result scan_source(const std::string& task_id, const BackupRequest& request);
    // 根据扫描结果提交或中止归档，并生成最终 Result。
    Result finish_archive(const std::string& task_id,
                          const BackupRequest& request,
                          const Result& scan_result);

    TaskManager& task_manager_;
    IScanner* scanner_;
    IFilter* filter_;
    IArchiveWriter* archive_writer_;
};

}  // namespace backup
