#pragma once

#include "common/backup_request.h"
#include "common/result.h"
#include "common/restore_request.h"
#include "scheduler/task.h"
#include "scheduler/task_manager.h"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace backup {

struct TaskSubmission {
    std::string task_id;
    std::string error_code;
    Result result;

    bool accepted() const { return !task_id.empty() && result.ok(); }
};

struct TaskSnapshot {
    Task task;
    std::string type;
    std::string created_at;
    std::string started_at;
    std::string finished_at;
};

struct TaskEvent {
    uint64_t id = 0;
    std::string task_id;
    std::string type;
    Task task;
};

class TaskRuntime {
public:
    TaskRuntime(TaskManager& task_manager,
                std::size_t worker_count = 2,
                std::size_t max_queued_tasks = 32);
    ~TaskRuntime();

    TaskRuntime(const TaskRuntime&) = delete;
    TaskRuntime& operator=(const TaskRuntime&) = delete;

    void start();
    void shutdown();

    TaskSubmission submit_backup(const BackupRequest& request);
    TaskSubmission submit_restore(const RestoreRequest& request);

    Task get_task(const std::string& task_id) const;
    std::vector<TaskSnapshot> list_tasks() const;
    std::vector<TaskEvent> get_events(const std::string& task_id,
                                      uint64_t after_id = 0) const;
    bool cancel_task(const std::string& task_id);

    std::size_t worker_count() const noexcept;
    std::size_t max_queued_tasks() const noexcept;
    std::size_t queued_task_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace backup
