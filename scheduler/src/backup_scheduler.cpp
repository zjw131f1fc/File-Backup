#include "scheduler/backup_scheduler.h"
#include "modules/scanner/scanner.h"
#include "modules/filter/filter.h"
#include "modules/archive_writer/archive_writer.h"

namespace backup {

BackupScheduler::BackupScheduler(
    TaskManager& task_manager,
    IScanner* scanner,
    IArchiveWriter* archive_writer
)
    : task_manager_(task_manager)
    , archive_writer_(archive_writer)
{
    if (!scanner) {
        default_scanner_ = create_scanner();
        scanner_ = default_scanner_.get();
    } else {
        scanner_ = scanner;
    }
}

Result BackupScheduler::run(const std::string& task_id, const BackupRequest& request) {
    // 更新状态为 RUNNING：正在创建筛选器
    {
        Progress p;
        p.stage = "creating_filter";
        p.current_path = request.source_path;
        task_manager_.update_progress(task_id, p);
    }

    // 创建筛选器（FilterRules 烘焙进实例）
    auto filter = create_filter(request.filter_rules);

    // 更新进度：正在创建归档写入器
    {
        Progress p;
        p.stage = "creating_archive";
        p.current_path = request.output_path;
        task_manager_.update_progress(task_id, p);
    }

    // 创建归档写入器；测试或上层可注入实现，默认使用工厂实例。
    std::unique_ptr<IArchiveWriter> default_writer;
    IArchiveWriter* writer = archive_writer_;
    if (!writer) {
        default_writer = create_archive(request.output_path);
        writer = default_writer.get();
    }

    if (!writer) {
        Result r;
        r.status = Status::FAILED;
        r.message = "failed to create archive writer";
        task_manager_.complete_task(task_id, r);
        return r;
    }

    // 更新进度：正在扫描和写入
    {
        Progress p;
        p.stage = "scanning";
        p.current_path = request.source_path;
        task_manager_.update_progress(task_id, p);
    }

    // 调用扫描器（内部：扫描 → 筛选 → 开源文件流 → 写入归档）
    // 进度回调返回 true 继续，返回 false 取消
    auto progress_cb = [this, &task_id](const Progress& p) -> bool {
        task_manager_.update_progress(task_id, p);
        // 检查任务是否被取消
        Task task = task_manager_.get_task(task_id);
        return task.status != TaskStatus::CANCELLED;
    };

    Result scan_result = scanner_->scan_and_backup(
        request.source_path,
        *filter,
        *writer,
        progress_cb
    );

    // 根据扫描结果决定提交或终止归档
    Result final_result;
    if (scan_result.ok()) {
        Progress p;
        p.stage = "committing_archive";
        task_manager_.update_progress(task_id, p);
        Result commit_result = writer->commit();
        if (commit_result.ok()) {
            final_result.status = Status::SUCCESS;
            final_result.message = "backup completed successfully";
        } else {
            writer->abort();
            final_result.status = Status::FAILED;
            final_result.message = "archive commit failed: " + commit_result.message;
            final_result.error_count = commit_result.error_count;
        }
    } else {
        writer->abort();
        if (scan_result.status == Status::PARTIAL_SUCCESS) {
            final_result.status = Status::FAILED;
            final_result.message = "backup partially failed, archive aborted";
            final_result.error_count = scan_result.error_count;
        } else {
            final_result = scan_result;
        }
    }

    task_manager_.complete_task(task_id, final_result);
    return final_result;
}

}  // namespace backup
