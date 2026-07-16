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

    // 执行还原任务
    Result run(const std::string& task_id, const RestoreRequest& request);

private:
    struct RestoreSummary {
        int error_count = 0;
        int warning_count = 0;
        bool cancelled = false;
    };

    void update_progress(const std::string& task_id,
                         const std::string& stage,
                         const std::string& current_path = {});
    Result validate_archive();
    RestoreSummary restore_entries(const std::string& task_id,
                                   const RestoreRequest& request);
    void restore_one_entry(const std::string& task_id,
                           const RestoreRequest& request,
                           RestoreSummary& summary);
    Result make_final_result(const RestoreSummary& summary) const;
    bool is_cancelled(const std::string& task_id) const;

    TaskManager& task_manager_;
    IArchiveReader* reader_;
    IRestorer* restorer_;
};

}  // namespace backup
