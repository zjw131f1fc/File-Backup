#pragma once

#include "common/filter_rules.h"
#include <string>

namespace backup {

struct BackupRequest {
    std::string source_path;
    // Resolved archive path used by the scheduler and archive writer.
    std::string output_path;
    // Optional API input. When set, TaskRuntime allocates output_path below it.
    std::string output_directory;
    std::string archive_name;
    FilterRules filter_rules;
};

}  // namespace backup
