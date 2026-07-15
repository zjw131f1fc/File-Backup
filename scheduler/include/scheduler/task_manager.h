#pragma once

#include "scheduler/task.h"
#include "common/backup_request.h"
#include "common/restore_request.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace backup {

class TaskManager {
public:
    // 创建备份任务，返回 task_id
    std::string create_backup_task(const BackupRequest& request);

    // 创建还原任务，返回 task_id
    std::string create_restore_task(const RestoreRequest& request);

    // 获取任务信息
    Task get_task(const std::string& task_id) const;

    // 取消任务（仅在 PENDING 或 RUNNING 时生效）
    bool cancel_task(const std::string& task_id);

    // 更新任务进度
    void update_progress(const std::string& task_id, const Progress& progress);

    // 标记任务完成
    void complete_task(const std::string& task_id, const Result& result);

    // 生成唯一 task_id
    static std::string generate_task_id();

private:
    std::unordered_map<std::string, Task> tasks_;
    mutable std::mutex mutex_;
    static std::atomic<uint64_t> id_counter_;
};

}  // namespace backup
