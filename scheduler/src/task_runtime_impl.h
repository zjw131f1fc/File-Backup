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

    // 初始化 Runtime 配置、工厂和任务事件观察者。
    Impl(TaskManager& manager,
         std::size_t workers,
         std::size_t max_queue,
         TaskRuntimeFactories task_factories);
    // 关闭 worker，并解除对 TaskManager 的观察。
    ~Impl();

    // 启动 worker 线程池。
    void start();
    // 停止接收任务、取消排队任务并等待线程退出。
    void shutdown();

    // 包装备份请求并提交到通用队列。
    TaskSubmission submit_backup(const BackupRequest& request);
    // 包装还原请求并提交到通用队列。
    TaskSubmission submit_restore(const RestoreRequest& request);
    // 执行通用入队流程，Job 中保存任务类型和完整请求。
    TaskSubmission submit(Job job);
    // 校验输出目录和文件名，并为备份选择最终输出路径。
    bool prepare_backup_output(BackupRequest& request, TaskSubmission& failure);

    // 在已持锁的前提下追加一个任务事件。
    void record_event_locked(const std::string& task_id,
                             const Task& task,
                             const std::string& change);
    // 加锁后调用 record_event_locked 的线程安全入口。
    void record_event(const std::string& task_id,
                      const Task& task,
                      const std::string& change);
    // 返回按提交顺序排列的任务展示快照。
    std::vector<TaskSnapshot> list_tasks() const;
    // 返回指定 ID 之后的任务事件。
    std::vector<TaskEvent> get_events(const std::string& task_id,
                                      uint64_t after_id) const;

    // 为缺省工厂绑定真实的五个子模块创建函数。
    void configure_default_factories();
    // worker 等待并执行队列中的 Job。
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
