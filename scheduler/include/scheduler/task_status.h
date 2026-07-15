#pragma once

#include "common/status.h"
#include <string>

namespace backup {

enum class TaskStatus {
    PENDING,
    RUNNING,
    SUCCESS,
    PARTIAL_SUCCESS,
    FAILED,
    CANCELLED,
};

}  // namespace backup
