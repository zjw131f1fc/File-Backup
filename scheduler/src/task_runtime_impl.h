#pragma once

#include "scheduler/task_runtime.h"
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace backup {

std::string task_runtime_timestamp_now();

// TaskRuntime 的私有实现。它保存队列、worker 和任务展示信息，公开头文件不暴露这些细节。
struct TaskRuntime::Impl {
    enum class TaskKind {
        BACKUP,
        RESTORE,
    };

    struct Job {
        TaskKind kind;
        std::string task_id;
        BackupRequest backup_request;
        RestoreRequest restore_request;
    };

    struct Metadata {
        std::string type;
        std::string output_path;
        std::string source_path;
        std::string created_at;
        std::string started_at;
        std::string finished_at;
    };

    Impl(TaskManager& manager,
         std::size_t workers,
         std::size_t max_queue,
         TaskRuntimeFactories task_factories);
    ~Impl();

    void start();
    void shutdown();

    TaskSubmission submit_backup(const BackupRequest& request);
    TaskSubmission submit_restore(const RestoreRequest& request);
    TaskSubmission submit(Job job);
    bool prepare_backup_output(BackupRequest& request, TaskSubmission& failure);

    void record_event_locked(const std::string& task_id,
                             const Task& task,
                             const std::string& change);
    void record_event(const std::string& task_id,
                      const Task& task,
                      const std::string& change);
    std::vector<TaskSnapshot> list_tasks() const;
    std::vector<TaskEvent> get_events(const std::string& task_id,
                                      uint64_t after_id) const;

    void configure_default_factories();
    void worker_loop();

    TaskManager& task_manager;
    const std::size_t worker_count_value;
    const std::size_t max_queued_tasks;
    TaskRuntimeFactories factories;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<Job> queue;
    std::vector<std::thread> workers;
    std::unordered_map<std::string, Metadata> metadata;
    std::vector<std::string> order;
    std::unordered_map<std::string, std::vector<TaskEvent>> events;
    uint64_t next_event_id = 1;
    bool started = false;
    bool stopping = false;
};

}  // namespace backup
