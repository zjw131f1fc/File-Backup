#pragma once

#include "scheduler/task.h"
#include "common/backup_request.h"
#include "common/restore_request.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <functional>

namespace backup {

class TaskManager {
public:
    // 任务状态变化通知：参数依次是任务 ID、最新任务快照和变化类型。
    using TaskObserver = std::function<void(const std::string&, const Task&, const std::string&)>;

    // 登记一个等待中的备份任务，只保存状态；请求正文由 TaskRuntime::Job 持有。
    std::string create_backup_task(const BackupRequest& request);

    // 登记一个等待中的还原任务，只保存状态；请求正文由 TaskRuntime::Job 持有。
    std::string create_restore_task(const RestoreRequest& request);

    // 获取任务当前快照；任务不存在时返回带失败信息的空任务。
    Task get_task(const std::string& task_id) const;

    // 将 PENDING 任务改为 RUNNING，并通知观察者。
    bool start_task(const std::string& task_id);

    // 取消 PENDING 或 RUNNING 任务；已结束的任务不能再次取消。
    bool cancel_task(const std::string& task_id);

    // 保存进度快照，并通知观察者把进度转发给 Runtime/API。
    void update_progress(const std::string& task_id, const Progress& progress);

    // 根据 Result 设置最终任务状态，并发送状态、结果和错误事件。
    void complete_task(const std::string& task_id, const Result& result);

    // 注册或清除状态变化观察者；观察者不会在锁内执行。
    void set_observer(TaskObserver observer);

    // 生成进程内唯一的任务 ID。
    static std::string generate_task_id();

private:
    std::unordered_map<std::string, Task> tasks_;
    mutable std::mutex mutex_;
    TaskObserver observer_;
    static std::atomic<uint64_t> id_counter_;
};

}  // namespace backup
