#include "scheduler/restore_scheduler.h"
#include "modules/archive_reader/archive_reader.h"
#include "modules/restore/restore.h"

namespace backup {

RestoreScheduler::RestoreScheduler(
    TaskManager& task_manager,
    IArchiveReader& archive_reader,
    IRestorer& restorer
)
    : task_manager_(task_manager)
    , reader_(&archive_reader)
    , restorer_(&restorer)
{
}

Result RestoreScheduler::run(const std::string& task_id, const RestoreRequest& request) {
    // 更新状态为 RUNNING：正在打开归档
    {
        Progress p;
        p.stage = "opening_archive";
        p.current_path = request.archive_path;
        task_manager_.update_progress(task_id, p);
    }

    // 校验归档
    {
        Progress p;
        p.stage = "validating_archive";
        task_manager_.update_progress(task_id, p);
    }

    Result validate_result = reader_->validate();
    if (!validate_result.ok()) {
        Result r;
        r.status = Status::FAILED;
        r.message = "archive validation failed: " + validate_result.message;
        task_manager_.complete_task(task_id, r);
        return r;
    }

    // 逐条恢复
    int error_count = 0;
    int warning_count = 0;
    Result final_result;

    while (reader_->has_next_entry()) {
        // 每次迭代前检查是否被取消
        Task task = task_manager_.get_task(task_id);
        if (task.status == TaskStatus::CANCELLED) {
            final_result.status = Status::CANCELLED;
            final_result.message = "restore cancelled by user";
            task_manager_.complete_task(task_id, final_result);
            return final_result;
        }

        EntryInfo entry_info;
        Result next_result = reader_->next_entry(entry_info);
        if (!next_result.ok()) {
            error_count++;
            continue;
        }

        Progress p;
        p.stage = "restoring";
        p.current_path = entry_info.path;
        p.processed_entries++;
        task_manager_.update_progress(task_id, p);

        // 恢复条目（恢复器内部处理所有类型和文件流）
        Result restore_result = restorer_->restore_entry(
            request.target_path, entry_info, *reader_, request.conflict_policy
        );

        if (!restore_result.ok()) {
            error_count++;
        } else {
            // 恢复元数据
            std::string target_path = request.target_path + "/" + entry_info.path;
            Result meta_result = restorer_->restore_metadata(target_path, entry_info);
            if (!meta_result.ok()) {
                warning_count++;
            }
        }
    }

    // 判断最终状态
    if (error_count == 0) {
        final_result.status = Status::SUCCESS;
        final_result.message = "restore completed successfully";
    } else {
        final_result.status = Status::PARTIAL_SUCCESS;
        final_result.message = "restore completed with " +
            std::to_string(error_count) + " errors";
        final_result.error_count = error_count;
    }
    final_result.warning_count = warning_count;

    task_manager_.complete_task(task_id, final_result);
    return final_result;
}

}  // namespace backup
