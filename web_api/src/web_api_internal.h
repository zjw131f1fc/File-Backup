#pragma once

#include "web_api/web_api.h"
#include "common/entry_type.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace backup::web_api_internal {

using json = nlohmann::json;

ApiResponse json_response(int status, const json& body);
ApiResponse error_response(int status,
                           const std::string& code,
                           const std::string& message);
ApiResponse submission_error_response(const TaskSubmission& submission);

bool is_directory(const std::string& path);
bool is_regular_file(const std::string& path);
bool is_readable(const std::string& path);
bool is_writable_directory(const std::filesystem::path& path);
bool valid_archive_name(const std::string& name);
bool valid_directory_name(const std::string& name);

std::filesystem::path normalized_path(const std::filesystem::path& path);
bool is_same_or_inside(const std::filesystem::path& child,
                       const std::filesystem::path& parent);
bool is_allowed_path(const std::filesystem::path& path,
                     const std::vector<std::string>& roots);

bool read_string(const json& object,
                 const char* key,
                 std::string& value);
bool read_filter_rules(const json& object, FilterRules& rules);

std::string entry_type_name(EntryType type);
bool parse_conflict_policy(const std::string& value, ConflictPolicy& policy);
std::string url_decode(const std::string& value);
std::string query_value(const std::string& query, const std::string& key);
std::string task_status_name(TaskStatus status);
bool is_terminal_status(TaskStatus status);

json task_json(const Task& task);
json snapshot_json(const TaskSnapshot& snapshot);
json filesystem_entry_json(const std::filesystem::directory_entry& entry);

}  // namespace backup::web_api_internal
