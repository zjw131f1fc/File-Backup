#include "web_api_internal.h"
#include <algorithm>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace backup::web_api_internal {

namespace {

bool read_string_array(const json& object,
                       const char* key,
                       std::vector<std::string>& values) {
    if (!object.contains(key)) {
        values.clear();
        return true;
    }
    if (!object[key].is_array()) return false;
    values.clear();
    for (const auto& item : object[key]) {
        if (!item.is_string()) return false;
        values.push_back(item.get<std::string>());
    }
    return true;
}

bool read_uid_array(const json& object,
                    const char* key,
                    std::vector<uid_t>& values) {
    if (!object.contains(key)) {
        values.clear();
        return true;
    }
    if (!object[key].is_array()) return false;
    values.clear();
    for (const auto& item : object[key]) {
        if (!item.is_number_unsigned()) return false;
        values.push_back(item.get<uid_t>());
    }
    return true;
}

}  // namespace

ApiResponse json_response(int status, const json& body) {
    return {status, "application/json; charset=utf-8", body.dump()};
}

ApiResponse error_response(int status,
                           const std::string& code,
                           const std::string& message) {
    return json_response(status, { {"error", {
        {"code", code},
        {"message", message},
        {"details", json::object()}
    }} });
}

ApiResponse submission_error_response(const TaskSubmission& submission) {
    int status = 500;
    if (submission.error_code == "OUTPUT_CONFLICT" ||
        submission.error_code == "OUTPUT_EXISTS") {
        status = 409;
    } else if (submission.error_code == "QUEUE_FULL") {
        status = 429;
    } else if (submission.error_code == "RUNTIME_STOPPED") {
        status = 503;
    }
    const std::string code = submission.error_code.empty()
        ? "TASK_SUBMISSION_FAILED" : submission.error_code;
    return error_response(status, code, submission.result.message);
}

bool is_directory(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_directory(
        std::filesystem::symlink_status(path, error)) && !error;
}

bool is_regular_file(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(
        std::filesystem::symlink_status(path, error)) && !error;
}

bool is_readable(const std::string& path) {
    return ::access(path.c_str(), R_OK) == 0;
}

bool is_writable_directory(const std::filesystem::path& path) {
    return std::filesystem::is_directory(path) && ::access(path.c_str(), W_OK) == 0;
}

bool valid_archive_name(const std::string& name) {
    if (name.empty()) return true;
    const std::filesystem::path path(name);
    return !path.is_absolute() && path.filename().string() == name &&
        name != "." && name != "..";
}

bool valid_directory_name(const std::string& name) {
    if (name.empty()) return false;
    const std::filesystem::path path(name);
    return !path.is_absolute() && path.filename().string() == name &&
        name != "." && name != "..";
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    const auto result = std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::absolute(path) : result;
}

namespace {

bool is_inside(const std::filesystem::path& child,
               const std::filesystem::path& parent) {
    const auto relative = normalized_path(child).lexically_relative(normalized_path(parent));
    return !relative.empty() && relative != "." && relative.native().rfind("..", 0) != 0;
}

}  // namespace

bool is_same_or_inside(const std::filesystem::path& child,
                       const std::filesystem::path& parent) {
    const auto normalized_child = normalized_path(child);
    const auto normalized_parent = normalized_path(parent);
    return normalized_child == normalized_parent ||
        is_inside(normalized_child, normalized_parent);
}

bool is_allowed_path(const std::filesystem::path& path,
                     const std::vector<std::string>& roots) {
    const auto normalized = normalized_path(path);
    for (const auto& root : roots) {
        const auto normalized_root = normalized_path(root);
        if (normalized == normalized_root || is_inside(normalized, normalized_root)) {
            return true;
        }
    }
    return false;
}

bool read_string(const json& object,
                 const char* key,
                 std::string& value) {
    if (!object.contains(key) || !object[key].is_string()) return false;
    value = object[key].get<std::string>();
    return !value.empty();
}

bool read_filter_rules(const json& object, FilterRules& rules) {
    if (!object.is_object() ||
        !read_string_array(object, "include_paths", rules.include_paths) ||
        !read_string_array(object, "exclude_paths", rules.exclude_paths) ||
        !read_string_array(object, "include_names", rules.include_names) ||
        !read_string_array(object, "exclude_names", rules.exclude_names) ||
        !read_uid_array(object, "include_uids", rules.include_uids)) {
        return false;
    }

    if (object.contains("include_types")) {
        if (!object["include_types"].is_array()) return false;
        rules.include_types.clear();
        static const std::vector<std::pair<std::string, EntryType>> types = {
            {"REGULAR_FILE", EntryType::REGULAR_FILE},
            {"DIRECTORY", EntryType::DIRECTORY},
            {"SYMBOLIC_LINK", EntryType::SYMBOLIC_LINK},
            {"HARD_LINK", EntryType::HARD_LINK},
            {"FIFO", EntryType::FIFO},
            {"CHARACTER_DEVICE", EntryType::CHARACTER_DEVICE},
            {"BLOCK_DEVICE", EntryType::BLOCK_DEVICE},
        };
        for (const auto& item : object["include_types"]) {
            if (!item.is_string()) return false;
            const std::string value = item.get<std::string>();
            const auto it = std::find_if(types.begin(), types.end(),
                [&value](const auto& candidate) { return candidate.first == value; });
            if (it == types.end()) return false;
            rules.include_types.push_back(it->second);
        }
    } else {
        rules.include_types.clear();
    }

    const auto read_int64 = [&object](const char* key, int64_t& value) {
        if (!object.contains(key)) return true;
        if (!object[key].is_number_integer()) return false;
        value = object[key].get<int64_t>();
        return value >= 0;
    };
    const auto read_uint64 = [&object](const char* key, uint64_t& value) {
        if (!object.contains(key)) return true;
        if (!object[key].is_number_unsigned()) return false;
        value = object[key].get<uint64_t>();
        return true;
    };
    if (!read_int64("newer_than_sec", rules.newer_than_sec) ||
        !read_int64("older_than_sec", rules.older_than_sec) ||
        !read_uint64("min_size", rules.min_size) ||
        !read_uint64("max_size", rules.max_size)) {
        return false;
    }
    return (rules.newer_than_sec == 0 || rules.older_than_sec == 0 ||
            rules.newer_than_sec < rules.older_than_sec) &&
        (rules.min_size == 0 || rules.max_size == 0 ||
         rules.min_size <= rules.max_size);
}

std::string entry_type_name(EntryType type) {
    switch (type) {
        case EntryType::REGULAR_FILE: return "REGULAR_FILE";
        case EntryType::DIRECTORY: return "DIRECTORY";
        case EntryType::SYMBOLIC_LINK: return "SYMBOLIC_LINK";
        case EntryType::HARD_LINK: return "HARD_LINK";
        case EntryType::FIFO: return "FIFO";
        case EntryType::CHARACTER_DEVICE: return "CHARACTER_DEVICE";
        case EntryType::BLOCK_DEVICE: return "BLOCK_DEVICE";
    }
    return "";
}

bool parse_conflict_policy(const std::string& value, ConflictPolicy& policy) {
    if (value == "SKIP") policy = ConflictPolicy::SKIP;
    else if (value == "OVERWRITE") policy = ConflictPolicy::OVERWRITE;
    else if (value == "RENAME") policy = ConflictPolicy::RENAME;
    else return false;
    return true;
}

std::string url_decode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    const auto hex = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hex(value[index + 1]);
            const int low = hex(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index] == '+' ? ' ' : value[index]);
    }
    return result;
}

std::string query_value(const std::string& query, const std::string& key) {
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto end = query.find('&', start);
        const auto part = query.substr(start, end == std::string::npos ? end : end - start);
        const auto separator = part.find('=');
        if (separator != std::string::npos &&
            url_decode(part.substr(0, separator)) == key) {
            return url_decode(part.substr(separator + 1));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

std::string task_status_name(TaskStatus status) {
    switch (status) {
        case TaskStatus::PENDING: return "PENDING";
        case TaskStatus::RUNNING: return "RUNNING";
        case TaskStatus::SUCCESS: return "SUCCESS";
        case TaskStatus::PARTIAL_SUCCESS: return "PARTIAL_SUCCESS";
        case TaskStatus::FAILED: return "FAILED";
        case TaskStatus::CANCELLED: return "CANCELLED";
    }
    return "FAILED";
}

bool is_terminal_status(TaskStatus status) {
    return status == TaskStatus::SUCCESS ||
        status == TaskStatus::PARTIAL_SUCCESS ||
        status == TaskStatus::FAILED ||
        status == TaskStatus::CANCELLED;
}

json task_json(const Task& task) {
    json result = nullptr;
    if (task.status != TaskStatus::PENDING && task.status != TaskStatus::RUNNING) {
        result = {
            {"status", task_status_name(task.result.status == Status::SUCCESS
                ? TaskStatus::SUCCESS
                : task.result.status == Status::PARTIAL_SUCCESS
                    ? TaskStatus::PARTIAL_SUCCESS
                    : task.result.status == Status::CANCELLED
                        ? TaskStatus::CANCELLED : TaskStatus::FAILED)},
            {"message", task.result.message},
            {"error_count", task.result.error_count},
            {"warning_count", task.result.warning_count}
        };
    }
    return {
        {"task_id", task.task_id},
        {"status", task_status_name(task.status)},
        {"progress", {
            {"stage", task.progress.stage},
            {"processed_entries", task.progress.processed_entries},
            {"processed_bytes", task.progress.processed_bytes},
            {"current_path", task.progress.current_path}
        }},
        {"result", result}
    };
}

json snapshot_json(const TaskSnapshot& snapshot) {
    auto result = task_json(snapshot.task);
    result["type"] = snapshot.type;
    if (snapshot.type == "backup" && !snapshot.source_path.empty()) {
        result["source_path"] = snapshot.source_path;
    }
    result["created_at"] = snapshot.created_at;
    result["started_at"] = snapshot.started_at.empty()
        ? json(nullptr) : json(snapshot.started_at);
    result["finished_at"] = snapshot.finished_at.empty()
        ? json(nullptr) : json(snapshot.finished_at);
    return result;
}

json filesystem_entry_json(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    const auto status = entry.symlink_status(error);
    json result = {
        {"name", entry.path().filename().string()},
        {"path", entry.path().string()},
        {"type", std::filesystem::is_directory(status) ? "directory" :
            std::filesystem::is_regular_file(status) ? "regular_file" : "other"}
    };
    if (!error && std::filesystem::is_regular_file(status)) {
        result["size"] = entry.file_size(error);
    }
    return result;
}

}  // namespace backup::web_api_internal
