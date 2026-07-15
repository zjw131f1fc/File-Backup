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
        IArchiveReader* archive_reader = nullptr,
        IRestorer* restorer = nullptr
    );

    // 执行还原任务
    Result run(const std::string& task_id, const RestoreRequest& request);

private:
    TaskManager& task_manager_;
    std::unique_ptr<IArchiveReader> default_reader_;
    std::unique_ptr<IRestorer> default_restorer_;
    IArchiveReader* reader_;
    IRestorer* restorer_;
};

}  // namespace backup
