#include "task_runtime_impl.h"
#include <filesystem>

namespace backup {

namespace {

bool valid_archive_name(const std::string& name) {
    if (name.empty()) return true;
    const std::filesystem::path path(name);
    return !path.is_absolute() && path.filename().string() == name &&
        name != "." && name != "..";
}

TaskSubmission failed_submission(const std::string& code, const std::string& message) {
    TaskSubmission submission;
    submission.error_code = code;
    submission.result.status = Status::FAILED;
    submission.result.message = message;
    return submission;
}

}  // namespace

// 提交入口只负责把备份/还原请求转换成队列中的 Job。
TaskSubmission TaskRuntime::Impl::submit_backup(const BackupRequest& request) {
    return submit(Job{TaskKind::BACKUP, "", request, {}});
}

TaskSubmission TaskRuntime::Impl::submit_restore(const RestoreRequest& request) {
    return submit(Job{TaskKind::RESTORE, "", {}, request});
}

TaskSubmission TaskRuntime::Impl::submit(Job job) {
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping) {
        return failed_submission("RUNTIME_STOPPED", "task runtime is shutting down");
    }
    if (queue.size() >= max_queued_tasks) {
        return failed_submission("QUEUE_FULL", "task queue is full");
    }

    if (job.kind == TaskKind::BACKUP) {
        TaskSubmission preparation_failure;
        if (!prepare_backup_output(job.backup_request, preparation_failure)) {
            return preparation_failure;
        }
    }

    if (job.kind == TaskKind::BACKUP && !job.backup_request.output_path.empty()) {
        for (const auto& item : metadata) {
            if (item.second.type != "backup" ||
                item.second.output_path != job.backup_request.output_path) {
                continue;
            }
            const Task task = task_manager.get_task(item.first);
            if (task.status == TaskStatus::PENDING || task.status == TaskStatus::RUNNING) {
                return failed_submission("OUTPUT_CONFLICT", "output archive is already in use");
            }
        }
    }

    if (job.kind == TaskKind::BACKUP) {
        job.task_id = task_manager.create_backup_task(job.backup_request);
    } else {
        job.task_id = task_manager.create_restore_task(job.restore_request);
    }

    const std::string submission_task_id = job.task_id;
    Metadata task_metadata;
    task_metadata.type = job.kind == TaskKind::BACKUP ? "backup" : "restore";
    task_metadata.output_path = job.backup_request.output_path;
    task_metadata.source_path = job.kind == TaskKind::BACKUP
        ? job.backup_request.source_path : std::string();
    task_metadata.created_at = task_runtime_timestamp_now();
    metadata.emplace(submission_task_id, task_metadata);
    order.push_back(submission_task_id);
    queue.push_back(std::move(job));
    record_event_locked(submission_task_id, task_manager.get_task(submission_task_id), "status");
    condition.notify_one();

    TaskSubmission submission;
    submission.task_id = submission_task_id;
    submission.result.status = Status::SUCCESS;
    submission.result.message = "task accepted";
    return submission;
}

bool TaskRuntime::Impl::prepare_backup_output(BackupRequest& request,
                                              TaskSubmission& failure) {
    if (request.output_directory.empty()) return true;

    if (!valid_archive_name(request.archive_name)) {
        failure = failed_submission(
            "INVALID_ARCHIVE_NAME",
            "archive_name must be a file name without a directory path");
        return false;
    }

    const std::filesystem::path directory(request.output_directory);
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        failure = failed_submission(
            "INVALID_PATH", "output_path must be an existing directory");
        return false;
    }

    const bool explicit_name = !request.archive_name.empty();
    const std::string requested_name = explicit_name ? request.archive_name : "backup.dat";
    const std::filesystem::path requested = directory / requested_name;

    const auto is_reserved = [this](const std::filesystem::path& candidate) {
        for (const auto& item : metadata) {
            if (item.second.type != "backup" ||
                item.second.output_path != candidate.string()) {
                continue;
            }
            const Task task = task_manager.get_task(item.first);
            if (task.status == TaskStatus::PENDING || task.status == TaskStatus::RUNNING) {
                return true;
            }
        }
        return false;
    };

    const auto is_occupied = [&is_reserved](const std::filesystem::path& candidate) {
        std::error_code exists_error;
        return std::filesystem::exists(candidate, exists_error) ||
            is_reserved(candidate);
    };

    std::filesystem::path selected = requested;
    if (explicit_name) {
        std::error_code exists_error;
        if (std::filesystem::exists(selected, exists_error)) {
            failure = failed_submission("OUTPUT_EXISTS", "output archive already exists");
            return false;
        }
        if (is_reserved(selected)) {
            failure = failed_submission("OUTPUT_CONFLICT", "output archive is already in use");
            return false;
        }
    } else {
        const auto stem = requested.stem().string();
        const auto extension = requested.extension().string();
        for (std::size_t suffix = 0; suffix < 100000; ++suffix) {
            selected = suffix == 0
                ? requested
                : directory / (stem + "-" + std::to_string(suffix) + extension);
            if (!is_occupied(selected)) break;
        }
        if (is_occupied(selected)) {
            failure = failed_submission(
                "OUTPUT_CONFLICT", "could not allocate a unique output archive");
            return false;
        }
    }

    request.output_path = selected.string();
    return true;
}

}  // namespace backup
