#include "scheduler/task_manager.h"
#include <chrono>
#include <sstream>
#include <utility>

namespace backup {

std::atomic<uint64_t> TaskManager::id_counter_{0};

// 用原子计数器和单调时钟拼出一个进程内唯一的任务 ID。
std::string TaskManager::generate_task_id() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream oss;
    oss << "task_" << ++id_counter_ << "_" << now;
    return oss.str();
}

// 只创建 PENDING 任务记录，真正的请求数据由 TaskRuntime 放进 Job 队列。
std::string TaskManager::create_backup_task(const BackupRequest&) {
    std::lock_guard<std::mutex> lock(mutex_);
    Task task;
    task.task_id = generate_task_id();
    task.status = TaskStatus::PENDING;
    task.progress.stage = "waiting";
    tasks_[task.task_id] = task;
    return task.task_id;
}

// 只创建 PENDING 任务记录，真正的请求数据由 TaskRuntime 放进 Job 队列。
std::string TaskManager::create_restore_task(const RestoreRequest&) {
    std::lock_guard<std::mutex> lock(mutex_);
    Task task;
    task.task_id = generate_task_id();
    task.status = TaskStatus::PENDING;
    task.progress.stage = "waiting";
    tasks_[task.task_id] = task;
    return task.task_id;
}

// 返回任务副本，调用方可以在不持有 TaskManager 锁的情况下读取快照。
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

// 只有等待中的任务可以开始；状态变化完成后再调用观察者，避免锁重入。
bool TaskManager::start_task(const std::string& task_id) {
    Task snapshot;
    TaskObserver observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end() || it->second.status != TaskStatus::PENDING) {
            return false;
        }
        it->second.status = TaskStatus::RUNNING;
        snapshot = it->second;
        observer = observer_;
    }
    if (observer) observer(task_id, snapshot, "status");
    return true;
}

// 把任务标记为取消；worker 仍可能正在运行，但后续会通过状态检查停止。
bool TaskManager::cancel_task(const std::string& task_id) {
    Task snapshot;
    TaskObserver observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) {
            return false;
        }
        if (it->second.status != TaskStatus::PENDING &&
            it->second.status != TaskStatus::RUNNING) {
            return false;
        }
        it->second.status = TaskStatus::CANCELLED;
        it->second.result.status = Status::CANCELLED;
        it->second.result.message = "cancelled by user";
        snapshot = it->second;
        observer = observer_;
    }
    if (observer) observer(task_id, snapshot, "status");
    return true;
}

// 更新进度并发布 progress 事件，不改变任务的生命周期状态。
void TaskManager::update_progress(const std::string& task_id, const Progress& progress) {
    Task snapshot;
    TaskObserver observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return;
        it->second.progress = progress;
        snapshot = it->second;
        observer = observer_;
    }
    if (observer) observer(task_id, snapshot, "progress");
}

// 保存执行结果，并把通用 Status 映射为 TaskStatus。
void TaskManager::complete_task(const std::string& task_id, const Result& result) {
    Task snapshot;
    TaskObserver observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return;
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
        snapshot = it->second;
        observer = observer_;
    }
    if (observer) {
        observer(task_id, snapshot, "status");
        observer(task_id, snapshot, "result");
        if (result.status == Status::FAILED ||
            result.status == Status::PARTIAL_SUCCESS) {
            observer(task_id, snapshot, "error");
        }
    }
}

// 设置观察者；传入空函数可以解除 Runtime 注册的事件回调。
void TaskManager::set_observer(TaskObserver observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observer_ = std::move(observer);
}

}  // namespace backup
