#pragma once

#include "common/result.h"
#include "common/restore_request.h"
#include "common/progress.h"
#include "scheduler/task_manager.h"
#include "modules/archive_reader/archive_reader.h"
#include "modules/restore/restore.h"
#include <memory>

namespace backup {

class RestoreScheduler {
public:
    explicit RestoreScheduler(
        TaskManager& task_manager,
        IArchiveReader& archive_reader,
        IRestorer& restorer
    );

    // 执行一次还原：校验归档、逐条恢复内容和元数据，并汇总结果。
    Result run(const std::string& task_id, const RestoreRequest& request);

private:
    // 还原过程中的计数器；不保存文件内容，只保存结果统计。
    struct RestoreSummary {
        int error_count = 0;
        int warning_count = 0;
        bool cancelled = false;
    };

    // 向 TaskManager 写入一个还原阶段的进度快照。
    void update_progress(const std::string& task_id,
                         const std::string& stage,
                         const std::string& current_path = {});
    // 调用 ArchiveReader 校验归档，并把失败原因转换成任务 Result。
    Result validate_archive();
    // 循环读取归档条目，直到全部处理完或任务被取消。
    RestoreSummary restore_entries(const std::string& task_id,
                                   const RestoreRequest& request);
    // 读取并恢复一个条目，然后尝试恢复该条目的元数据。
    void restore_one_entry(const std::string& task_id,
                           const RestoreRequest& request,
                           RestoreSummary& summary);
    // 把统计结果转换为 SUCCESS、PARTIAL_SUCCESS 或 CANCELLED。
    Result make_final_result(const RestoreSummary& summary) const;
    // 查询 TaskManager 判断任务是否已经被取消。
    bool is_cancelled(const std::string& task_id) const;

    TaskManager& task_manager_;
    IArchiveReader* reader_;
    IRestorer* restorer_;
};

}  // namespace backup
