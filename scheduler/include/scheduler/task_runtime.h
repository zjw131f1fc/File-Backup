#pragma once

#include "common/backup_request.h"
#include "common/result.h"
#include "common/restore_request.h"
#include "scheduler/task.h"
#include "scheduler/task_manager.h"
#include <cstddef>
#include <memory>
#include <string>

namespace backup {

struct TaskSubmission {
    std::string task_id;
    Result result;

    bool accepted() const { return !task_id.empty() && result.ok(); }
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
    bool cancel_task(const std::string& task_id);

    std::size_t worker_count() const noexcept;
    std::size_t max_queued_tasks() const noexcept;
    std::size_t queued_task_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace backup
