#pragma once

#include "web_api/web_api.h"
#include "common/entry_type.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace backup::web_api_internal {

using json = nlohmann::json;

// 把 JSON 值包装成统一的 HTTP 成功响应。
ApiResponse json_response(int status, const json& body);
// 构造统一错误格式：error.code 和 error.message。
ApiResponse error_response(int status,
                           const std::string& code,
                           const std::string& message);
// 把 Runtime 的提交失败转换成 HTTP 错误响应。
ApiResponse submission_error_response(const TaskSubmission& submission);

// 文件系统状态检查。
bool is_directory(const std::string& path);
bool is_regular_file(const std::string& path);
bool is_readable(const std::string& path);
bool is_writable_directory(const std::filesystem::path& path);
bool valid_archive_name(const std::string& name);
bool valid_directory_name(const std::string& name);

// 路径规范化与路径范围检查。
std::filesystem::path normalized_path(const std::filesystem::path& path);
bool is_same_or_inside(const std::filesystem::path& child,
                       const std::filesystem::path& parent);
bool is_allowed_path(const std::filesystem::path& path,
                     const std::vector<std::string>& roots);

// 从 JSON 读取字符串和过滤规则。
bool read_string(const json& object,
                 const char* key,
                 std::string& value);
bool read_filter_rules(const json& object, FilterRules& rules);

// 枚举值和 URL/查询参数转换。
std::string entry_type_name(EntryType type);
bool parse_conflict_policy(const std::string& value, ConflictPolicy& policy);
std::string url_decode(const std::string& value);
std::string query_value(const std::string& query, const std::string& key);
std::string task_status_name(TaskStatus status);
bool is_terminal_status(TaskStatus status);

// 把任务、任务快照和目录条目转换成 API JSON。
json task_json(const Task& task);
json snapshot_json(const TaskSnapshot& snapshot);
json filesystem_entry_json(const std::filesystem::directory_entry& entry);

}  // namespace backup::web_api_internal
