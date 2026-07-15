#pragma once

#include "common/conflict_policy.h"
#include <string>

namespace backup {

struct RestoreRequest {
    std::string archive_path;
    std::string target_path;
    ConflictPolicy conflict_policy = ConflictPolicy::SKIP;
};

}  // namespace backup
