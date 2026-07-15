#pragma once

#include "common/filter_rules.h"
#include <string>

namespace backup {

struct BackupRequest {
    std::string source_path;
    std::string output_path;
    FilterRules filter_rules;
};

}  // namespace backup
