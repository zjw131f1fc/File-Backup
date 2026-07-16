#pragma once

#include "common/backup_request.h"
#include "common/result.h"
#include "common/restore_request.h"
#include "scheduler/task.h"
#include "scheduler/task_manager.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace backup {

class IScanner;
class IFilter;
class IArchiveWriter;
class IArchiveReader;
class IRestorer;

struct TaskRuntimeFactories {
    std::function<std::unique_ptr<IScanner>()> scanner;
    std::function<std::unique_ptr<IFilter>(const FilterRules&)> filter;
    std::function<std::unique_ptr<IArchiveWriter>(const std::string&)> archive_writer;
    std::function<std::unique_ptr<IArchiveReader>(const std::string&)> archive_reader;
    std::function<std::unique_ptr<IRestorer>()> restorer;
};

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
    std::string source_path;
};

struct TaskEvent {
    uint64_t id = 0;
    std::string task_id;
    std::string type;
    Task task;
};

// 任务运行时：维护待执行队列和固定数量的后台 worker。
// 它不直接执行备份/还原，而是把 Job 分发给对应的 Scheduler。
class TaskRuntime {
public:
    // 创建 Runtime；worker_count 是并发 worker 数，max_queued_tasks 是队列容量。
    TaskRuntime(TaskManager& task_manager,
                std::size_t worker_count = 2,
                std::size_t max_queued_tasks = 32,
                TaskRuntimeFactories factories = {});
    ~TaskRuntime();

    TaskRuntime(const TaskRuntime&) = delete;
    TaskRuntime& operator=(const TaskRuntime&) = delete;

    // 启动固定数量的后台 worker；重复调用不会重复创建线程。
    void start();
    // 停止接收新任务，取消队列中尚未开始的任务，并等待 worker 退出。
    void shutdown();

    // 校验并提交备份请求，成功时立即返回 task_id，不等待备份完成。
    TaskSubmission submit_backup(const BackupRequest& request);
    // 校验并提交还原请求，成功时立即返回 task_id，不等待还原完成。
    TaskSubmission submit_restore(const RestoreRequest& request);

    // 查询单个任务的状态、进度和结果。
    Task get_task(const std::string& task_id) const;
    // 按提交顺序返回所有任务的展示快照。
    std::vector<TaskSnapshot> list_tasks() const;
    // 返回指定任务在 after_id 之后产生的事件，用于 SSE/增量查询。
    std::vector<TaskEvent> get_events(const std::string& task_id,
                                      uint64_t after_id = 0) const;
    // 请求取消任务；实际运行中的子模块会在检查到取消状态后结束。
    bool cancel_task(const std::string& task_id);

    // 返回配置的 worker 数量。
    std::size_t worker_count() const noexcept;
    // 返回队列最大容量。
    std::size_t max_queued_tasks() const noexcept;
    // 返回当前尚未被 worker 取走的任务数。
    std::size_t queued_task_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace backup
