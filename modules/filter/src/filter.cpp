#include "modules/filter/filter.h"

namespace backup {

namespace {

// ===== glob 模式匹配（支持 * 和 ?） =====
bool glob_match(const char* pattern, const char* str) {
    if (*pattern == '\0' && *str == '\0') return true;
    if (*pattern == '\0') return false;
    if (*str == '\0') {
        while (*pattern == '*') ++pattern;
        return *pattern == '\0';
    }

    if (*pattern == '*') {
        while (*pattern == '*') ++pattern;
        if (*pattern == '\0') return true;
        while (*str) {
            if (glob_match(pattern, str)) return true;
            ++str;
        }
        return false;
    }

    if (*pattern == '?' || *pattern == *str) {
        return glob_match(pattern + 1, str + 1);
    }

    return false;
}

bool glob_match(const std::string& pattern, const std::string& str) {
    return glob_match(pattern.c_str(), str.c_str());
}

// 检查模式是否包含 glob 通配符
bool is_glob_pattern(const std::string& pattern) {
    return pattern.find('*') != std::string::npos ||
           pattern.find('?') != std::string::npos;
}

// 路径匹配规则：
// - 以 / 结尾 → 目录前缀匹配（匹配该目录下所有内容）
// - 含 * 或 ? → glob 模式匹配
// - 其他 → 精确前缀匹配
bool path_matches(const std::string& path, const std::string& pattern) {
    if (!pattern.empty() && pattern.back() == '/') {
        // 目录前缀匹配：pattern 以 / 结尾，匹配该目录下所有内容
        if (path.size() < pattern.size()) return false;
        if (path.compare(0, pattern.size(), pattern) != 0) return false;
        return true;  // pattern 末尾 / 已匹配目录分隔符
    }
    if (is_glob_pattern(pattern)) {
        return glob_match(pattern, path);
    }
    // 普通前缀匹配
    if (path.size() < pattern.size()) return false;
    if (path.compare(0, pattern.size(), pattern) != 0) return false;
    if (path.size() == pattern.size()) return true;
    if (path[pattern.size()] == '/') return true;
    return false;
}

// 获取文件名部分
std::string filename(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

}  // anonymous namespace

class Filter : public IFilter {
public:
    explicit Filter(const FilterRules& rules) : rules_(rules) {}

    bool should_include(const EntryInfo& entry) override {
        // 1. 路径排除（最高优先级）
        for (const auto& pattern : rules_.exclude_paths) {
            if (path_matches(entry.path, pattern)) {
                return false;
            }
        }

        // 2. 路径包含
        if (!rules_.include_paths.empty()) {
            bool matched = false;
            for (const auto& pattern : rules_.include_paths) {
                if (path_matches(entry.path, pattern)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) return false;
        }

        // 3. 类型筛选
        if (!rules_.include_types.empty()) {
            bool matched = false;
            for (const auto& type : rules_.include_types) {
                if (entry.type == type) {
                    matched = true;
                    break;
                }
            }
            if (!matched) return false;
        }

        // 4. 名称排除
        std::string fname = filename(entry.path);
        for (const auto& pattern : rules_.exclude_names) {
            if (glob_match(pattern, fname)) return false;
        }

        // 5. 名称包含
        if (!rules_.include_names.empty()) {
            bool matched = false;
            for (const auto& pattern : rules_.include_names) {
                if (glob_match(pattern, fname)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) return false;
        }

        // 6. 时间筛选
        if (rules_.newer_than_sec > 0 && entry.mtime_sec <= rules_.newer_than_sec) {
            return false;
        }
        if (rules_.older_than_sec > 0 && entry.mtime_sec >= rules_.older_than_sec) {
            return false;
        }

        // 7. 尺寸筛选
        if (rules_.min_size > 0 && entry.size < rules_.min_size) return false;
        if (rules_.max_size > 0 && entry.size > rules_.max_size) return false;

        // 8. 用户筛选
        if (!rules_.include_uids.empty()) {
            bool matched = false;
            for (const auto& uid : rules_.include_uids) {
                if (entry.uid == uid) {
                    matched = true;
                    break;
                }
            }
            if (!matched) return false;
        }

        return true;
    }

private:
    FilterRules rules_;
};

std::unique_ptr<IFilter> create_filter(const FilterRules& rules) {
    return std::make_unique<Filter>(rules);
}

}  // namespace backup
