#pragma once

#include "common/status.h"
#include <string>

namespace backup {

struct Result {
    Status status = Status::SUCCESS;
    std::string message;
    int error_count = 0;
    int warning_count = 0;

    bool ok() const { return status == Status::SUCCESS; }
};

}  // namespace backup
