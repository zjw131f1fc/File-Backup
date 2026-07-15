#pragma once

#include "scheduler/task_status.h"
#include "common/result.h"
#include "common/progress.h"
#include <string>

namespace backup {

struct Task {
    std::string task_id;
    TaskStatus status = TaskStatus::PENDING;
    Progress progress;
    Result result;
};

}  // namespace backup
