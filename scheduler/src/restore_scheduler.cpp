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

// 还原主流程：打开并校验归档 -> 逐条恢复 -> 汇总结果。
Result RestoreScheduler::run(const std::string& task_id, const RestoreRequest& request) {
    update_progress(task_id, "opening_archive", request.archive_path);
    update_progress(task_id, "validating_archive");

    const Result validation_result = validate_archive();
    if (!validation_result.ok()) {
        task_manager_.complete_task(task_id, validation_result);
        return validation_result;
    }

    const RestoreSummary summary = restore_entries(task_id, request);
    const Result final_result = make_final_result(summary);
    task_manager_.complete_task(task_id, final_result);
    return final_result;
}

void RestoreScheduler::update_progress(const std::string& task_id,
                                       const std::string& stage,
                                       const std::string& current_path) {
    Progress progress;
    progress.stage = stage;
    progress.current_path = current_path;
    task_manager_.update_progress(task_id, progress);
}

Result RestoreScheduler::validate_archive() {
    const Result validation_result = reader_->validate();
    if (validation_result.ok()) {
        return validation_result;
    }

    Result result;
    result.status = Status::FAILED;
    result.message = "archive validation failed: " + validation_result.message;
    return result;
}

RestoreScheduler::RestoreSummary RestoreScheduler::restore_entries(
    const std::string& task_id,
    const RestoreRequest& request) {
    RestoreSummary summary;
    while (reader_->has_next_entry()) {
        if (is_cancelled(task_id)) {
            summary.cancelled = true;
            return summary;
        }
        restore_one_entry(task_id, request, summary);
    }
    return summary;
}

void RestoreScheduler::restore_one_entry(const std::string& task_id,
                                         const RestoreRequest& request,
                                         RestoreSummary& summary) {
    EntryInfo entry_info;
    const Result next_result = reader_->next_entry(entry_info);
    if (!next_result.ok()) {
        ++summary.error_count;
        return;
    }

    Progress progress = task_manager_.get_task(task_id).progress;
    progress.stage = "restoring";
    progress.current_path = entry_info.path;
    ++progress.processed_entries;
    if (entry_info.type == EntryType::REGULAR_FILE) {
        progress.processed_bytes += entry_info.size;
    }
    task_manager_.update_progress(task_id, progress);

    // 恢复器内部处理文件、目录等条目类型；成功后再恢复元数据。
    const Result restore_result = restorer_->restore_entry(
        request.target_path, entry_info, *reader_, request.conflict_policy
    );
    if (!restore_result.ok()) {
        ++summary.error_count;
        return;
    }

    const std::string target_path = request.target_path + "/" + entry_info.path;
    if (!restorer_->restore_metadata(target_path, entry_info).ok()) {
        ++summary.warning_count;
    }
}

Result RestoreScheduler::make_final_result(const RestoreSummary& summary) const {
    Result result;
    if (summary.cancelled) {
        result.status = Status::CANCELLED;
        result.message = "restore cancelled by user";
        return result;
    }

    if (summary.error_count == 0) {
        result.status = Status::SUCCESS;
        result.message = "restore completed successfully";
    } else {
        result.status = Status::PARTIAL_SUCCESS;
        result.message = "restore completed with " +
            std::to_string(summary.error_count) + " errors";
        result.error_count = summary.error_count;
    }
    result.warning_count = summary.warning_count;
    return result;
}

bool RestoreScheduler::is_cancelled(const std::string& task_id) const {
    return task_manager_.get_task(task_id).status == TaskStatus::CANCELLED;
}

}  // namespace backup
