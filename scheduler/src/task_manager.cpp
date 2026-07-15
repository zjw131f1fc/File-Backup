#include "scheduler/task_manager.h"
#include <chrono>
#include <sstream>

namespace backup {

std::atomic<uint64_t> TaskManager::id_counter_{0};

std::string TaskManager::generate_task_id() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream oss;
    oss << "task_" << ++id_counter_ << "_" << now;
    return oss.str();
}

std::string TaskManager::create_backup_task(const BackupRequest& request) {
    (void)request;
    std::lock_guard<std::mutex> lock(mutex_);
    Task task;
    task.task_id = generate_task_id();
    task.status = TaskStatus::PENDING;
    task.progress.stage = "waiting";
    tasks_[task.task_id] = task;
    return task.task_id;
}

std::string TaskManager::create_restore_task(const RestoreRequest& request) {
    (void)request;
    std::lock_guard<std::mutex> lock(mutex_);
    Task task;
    task.task_id = generate_task_id();
    task.status = TaskStatus::PENDING;
    task.progress.stage = "waiting";
    tasks_[task.task_id] = task;
    return task.task_id;
}

Task TaskManager::get_task(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        Task empty;
        empty.task_id = "";
        empty.status = TaskStatus::FAILED;
        empty.result.status = Status::FAILED;
        empty.result.message = "task not found: " + task_id;
        return empty;
    }
    return it->second;
}

bool TaskManager::start_task(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end() || it->second.status != TaskStatus::PENDING) {
        return false;
    }
    it->second.status = TaskStatus::RUNNING;
    return true;
}

bool TaskManager::cancel_task(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        return false;
    }
    if (it->second.status == TaskStatus::PENDING ||
        it->second.status == TaskStatus::RUNNING) {
        it->second.status = TaskStatus::CANCELLED;
        it->second.result.status = Status::CANCELLED;
        it->second.result.message = "cancelled by user";
        return true;
    }
    return false;
}

void TaskManager::update_progress(const std::string& task_id, const Progress& progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        it->second.progress = progress;
    }
}

void TaskManager::complete_task(const std::string& task_id, const Result& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        return;
    }
    if (it->second.status == TaskStatus::CANCELLED &&
        result.status != Status::CANCELLED) {
        return;
    }
    it->second.result = result;
    switch (result.status) {
        case Status::SUCCESS:
            it->second.status = TaskStatus::SUCCESS;
            break;
        case Status::PARTIAL_SUCCESS:
            it->second.status = TaskStatus::PARTIAL_SUCCESS;
            break;
        case Status::CANCELLED:
            it->second.status = TaskStatus::CANCELLED;
            break;
        case Status::FAILED:
            it->second.status = TaskStatus::FAILED;
            break;
    }
}

}  // namespace backup
