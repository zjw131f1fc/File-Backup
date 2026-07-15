#pragma once

#include <string>
#include <cstdint>

namespace backup {

struct Progress {
    std::string stage;
    uint64_t processed_entries = 0;
    uint64_t processed_bytes = 0;
    std::string current_path;
};

}  // namespace backup
