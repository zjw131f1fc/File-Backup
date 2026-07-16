#include "modules/filter/filter.h"
#include <filesystem>
#include <fnmatch.h>
#include <algorithm>

namespace {

bool matches_path_pattern(const std::string& path, const std::string& pattern) {
    if (fnmatch(pattern.c_str(), path.c_str(), 0) == 0) {
        return true;
    }
    return !pattern.empty() && pattern.back() == '/' &&
        path.rfind(pattern, 0) == 0;
}

bool matches_any(const std::string& path, const std::vector<std::string>& patterns) {
    return std::any_of(patterns.begin(), patterns.end(),
        [&path](const std::string& pattern) {
            return matches_path_pattern(path, pattern);
        });
}

}  // namespace

namespace backup {

class StubFilter : public IFilter {
public:
    explicit StubFilter(const FilterRules& rules) : rules_(rules) {}

    bool should_include(const EntryInfo& entry) override {
        if (!rules_.include_paths.empty() &&
            !matches_any(entry.path, rules_.include_paths)) {
            return false;
        }
        if (matches_any(entry.path, rules_.exclude_paths)) {
            return false;
        }

        if (!rules_.include_types.empty() &&
            std::find(rules_.include_types.begin(), rules_.include_types.end(),
                      entry.type) == rules_.include_types.end()) {
            return false;
        }

        const std::string name = std::filesystem::path(entry.path).filename().string();
        if (!rules_.include_names.empty() &&
            !matches_any(name, rules_.include_names)) {
            return false;
        }
        if (matches_any(name, rules_.exclude_names)) {
            return false;
        }

        if (rules_.newer_than_sec != 0 &&
            entry.mtime_sec < rules_.newer_than_sec) {
            return false;
        }
        if (rules_.older_than_sec != 0 &&
            entry.mtime_sec > rules_.older_than_sec) {
            return false;
        }
        if (rules_.min_size != 0 && entry.size < rules_.min_size) {
            return false;
        }
        if (rules_.max_size != 0 && entry.size > rules_.max_size) {
            return false;
        }
        if (!rules_.include_uids.empty() &&
            std::find(rules_.include_uids.begin(), rules_.include_uids.end(),
                      entry.uid) == rules_.include_uids.end()) {
            return false;
        }
        return true;
    }

private:
    FilterRules rules_;
};

std::unique_ptr<IFilter> create_filter(const FilterRules& rules) {
    return std::make_unique<StubFilter>(rules);
}

}  // namespace backup
