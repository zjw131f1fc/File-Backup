#include "scheduler/backup_scheduler.h"
#include "modules/scanner/scanner.h"
#include "modules/filter/filter.h"
#include "modules/archive_writer/archive_writer.h"

namespace backup {

BackupScheduler::BackupScheduler(
    TaskManager& task_manager,
    IScanner& scanner,
    IFilter& filter,
    IArchiveWriter& archive_writer
)
    : task_manager_(task_manager)
    , scanner_(&scanner)
    , filter_(&filter)
    , archive_writer_(&archive_writer)
{
}

// 备份主流程：更新阶段 -> 扫描写入 -> 提交归档 -> 记录最终结果。
Result BackupScheduler::run(const std::string& task_id, const BackupRequest& request) {
    update_progress(task_id, "creating_filter", request.source_path);
    update_progress(task_id, "creating_archive", request.output_path);
    update_progress(task_id, "scanning", request.source_path);

    const Result scan_result = scan_source(task_id, request);
    const Result final_result = finish_archive(task_id, request, scan_result);
    task_manager_.complete_task(task_id, final_result);
    return final_result;
}

// 统一构造阶段性进度，避免 run() 重复填写 Progress 字段。
void BackupScheduler::update_progress(const std::string& task_id,
                                      const std::string& stage,
                                      const std::string& current_path) {
    Progress progress;
    progress.stage = stage;
    progress.current_path = current_path;
    task_manager_.update_progress(task_id, progress);
}

// 让 Scanner 负责遍历源目录；回调同时把实时进度交给 TaskManager，并检查取消。
Result BackupScheduler::scan_source(const std::string& task_id,
                                    const BackupRequest& request) {
    // 扫描器内部负责“扫描 -> 筛选 -> 写入归档”。回调返回 false 表示取消。
    const auto progress_callback = [this, &task_id](const Progress& progress) {
        task_manager_.update_progress(task_id, progress);
        return task_manager_.get_task(task_id).status != TaskStatus::CANCELLED;
    };

    return scanner_->scan_and_backup(
        request.source_path,
        *filter_,
        *archive_writer_,
        progress_callback
    );
}

// 扫描成功才允许 commit；扫描失败或提交失败都必须 abort，避免留下不完整归档。
Result BackupScheduler::finish_archive(const std::string& task_id,
                                        const BackupRequest& request,
                                        const Result& scan_result) {
    if (!scan_result.ok()) {
        archive_writer_->abort();
        if (scan_result.status == Status::PARTIAL_SUCCESS) {
            Result result;
            result.status = Status::FAILED;
            result.message = "backup partially failed, archive aborted";
            result.error_count = scan_result.error_count;
            return result;
        }
        return scan_result;
    }

    Progress progress = task_manager_.get_task(task_id).progress;
    progress.stage = "committing_archive";
    progress.current_path = request.output_path;
    task_manager_.update_progress(task_id, progress);

    const Result commit_result = archive_writer_->commit();
    if (commit_result.ok()) {
        Result result;
        result.status = Status::SUCCESS;
        result.message = "backup completed successfully";
        return result;
    }

    archive_writer_->abort();
    Result result;
    result.status = Status::FAILED;
    result.message = "archive commit failed: " + commit_result.message;
    result.error_count = commit_result.error_count;
    return result;
}

}  // namespace backup
