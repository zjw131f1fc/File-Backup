#include "task_runtime_impl.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace backup {

std::string task_runtime_timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc {};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

namespace {

bool is_terminal_status(TaskStatus status) {
    return status == TaskStatus::SUCCESS ||
        status == TaskStatus::PARTIAL_SUCCESS ||
        status == TaskStatus::FAILED ||
        status == TaskStatus::CANCELLED;
}

}  // namespace

// 事件记录为 API 查询和 SSE 推送提供历史快照。
void TaskRuntime::Impl::record_event_locked(const std::string& task_id,
                                             const Task& task,
                                             const std::string& change) {
    auto metadata_it = metadata.find(task_id);
    if (metadata_it == metadata.end()) return;
    if (task.status == TaskStatus::RUNNING && metadata_it->second.started_at.empty()) {
        metadata_it->second.started_at = task_runtime_timestamp_now();
    }
    if (is_terminal_status(task.status) && metadata_it->second.finished_at.empty()) {
        metadata_it->second.finished_at = task_runtime_timestamp_now();
    }
    events[task_id].push_back(TaskEvent{next_event_id++, task_id, change, task});
}

void TaskRuntime::Impl::record_event(const std::string& task_id,
                                     const Task& task,
                                     const std::string& change) {
    std::lock_guard<std::mutex> lock(mutex);
    record_event_locked(task_id, task, change);
}

std::vector<TaskSnapshot> TaskRuntime::Impl::list_tasks() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<TaskSnapshot> result;
    result.reserve(order.size());
    for (const auto& task_id : order) {
        const auto metadata_it = metadata.find(task_id);
        if (metadata_it == metadata.end()) continue;
        const auto& item = metadata_it->second;
        result.push_back(TaskSnapshot{
            task_manager.get_task(task_id), item.type, item.created_at,
            item.started_at, item.finished_at, item.source_path
        });
    }
    return result;
}

std::vector<TaskEvent> TaskRuntime::Impl::get_events(const std::string& task_id,
                                                     uint64_t after_id) const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<TaskEvent> result;
    const auto it = events.find(task_id);
    if (it == events.end()) return result;
    for (const auto& event : it->second) {
        if (event.id > after_id) result.push_back(event);
    }
    return result;
}

}  // namespace backup
