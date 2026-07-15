#include "web_api/web_api.h"
#include "common/entry_type.h"
#include "modules/archive_reader/archive_reader.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace backup {

namespace {

using json = nlohmann::json;

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

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::absolute(path) : result;
}

bool is_inside(const std::filesystem::path& child,
               const std::filesystem::path& parent) {
    const auto relative = normalized_path(child).lexically_relative(normalized_path(parent));
    return !relative.empty() && relative != "." && relative.native().rfind("..", 0) != 0;
}

bool read_string(const json& object,
                 const char* key,
                 std::string& value) {
    if (!object.contains(key) || !object[key].is_string()) {
        return false;
    }
    value = object[key].get<std::string>();
    return !value.empty();
}

bool read_string_array(const json& object,
                       const char* key,
                       std::vector<std::string>& values) {
    if (!object.contains(key)) {
        values.clear();
        return true;
    }
    if (!object[key].is_array()) {
        return false;
    }
    values.clear();
    for (const auto& item : object[key]) {
        if (!item.is_string()) {
            return false;
        }
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
    if (!object[key].is_array()) {
        return false;
    }
    values.clear();
    for (const auto& item : object[key]) {
        if (!item.is_number_unsigned()) {
            return false;
        }
        values.push_back(item.get<uid_t>());
    }
    return true;
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
        if (!object["include_types"].is_array()) {
            return false;
        }
        rules.include_types.clear();
        for (const auto& item : object["include_types"]) {
            if (!item.is_string()) {
                return false;
            }
            const std::string value = item.get<std::string>();
            static const std::vector<std::pair<std::string, EntryType>> types = {
                {"REGULAR_FILE", EntryType::REGULAR_FILE},
                {"DIRECTORY", EntryType::DIRECTORY},
                {"SYMBOLIC_LINK", EntryType::SYMBOLIC_LINK},
                {"HARD_LINK", EntryType::HARD_LINK},
                {"FIFO", EntryType::FIFO},
                {"CHARACTER_DEVICE", EntryType::CHARACTER_DEVICE},
                {"BLOCK_DEVICE", EntryType::BLOCK_DEVICE},
            };
            const auto it = std::find_if(types.begin(), types.end(),
                [&value](const auto& candidate) { return candidate.first == value; });
            if (it == types.end()) {
                return false;
            }
            rules.include_types.push_back(it->second);
        }
    } else {
        rules.include_types.clear();
    }

    auto read_int64 = [&object](const char* key, int64_t& value) {
        if (!object.contains(key)) return true;
        if (!object[key].is_number_integer()) return false;
        value = object[key].get<int64_t>();
        return value >= 0;
    };
    auto read_uint64 = [&object](const char* key, uint64_t& value) {
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
    if (rules.newer_than_sec != 0 && rules.older_than_sec != 0 &&
        rules.newer_than_sec >= rules.older_than_sec) {
        return false;
    }
    if (rules.min_size != 0 && rules.max_size != 0 &&
        rules.min_size > rules.max_size) {
        return false;
    }
    return true;
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

}  // namespace

WebApi::WebApi(TaskRuntime& runtime, ApiConfig config)
    : runtime_(runtime), config_(std::move(config)) {
    if (config_.allowed_roots.empty()) {
        config_.allowed_roots.push_back(std::filesystem::current_path().string());
    }
}

ApiResponse WebApi::handle(const std::string& method,
                           const std::string& target,
                           const std::string& body) {
    const auto query_position = target.find('?');
    const std::string path = target.substr(0, query_position);

    if (method == "GET" && path == "/api/health") {
        return json_response(200, {{"status", "ok"}, {"service", "backup-web"}});
    }
    if (method == "GET" && path == "/api/capabilities") {
        json entry_types = json::array();
        for (const auto type : {EntryType::REGULAR_FILE, EntryType::DIRECTORY,
                                EntryType::SYMBOLIC_LINK, EntryType::HARD_LINK,
                                EntryType::FIFO, EntryType::CHARACTER_DEVICE,
                                EntryType::BLOCK_DEVICE}) {
            entry_types.push_back(entry_type_name(type));
        }
        return json_response(200, {
            {"entry_types", entry_types},
            {"conflict_policies", {"SKIP", "OVERWRITE", "RENAME"}},
            {"filter_rules", {"include_paths", "exclude_paths", "include_types",
                "include_names", "exclude_names", "newer_than_sec", "older_than_sec",
                "min_size", "max_size", "include_uids"}},
            {"progress_events", true},
            {"concurrency", {
                {"enabled", true},
                {"worker_count", runtime_.worker_count()},
                {"max_queued_tasks", runtime_.max_queued_tasks()}
            }}
        });
    }

    if (method != "POST" || (path != "/api/backup" && path != "/api/restore")) {
        return error_response(404, "NOT_FOUND", "API endpoint not found");
    }

    json request;
    try {
        request = json::parse(body);
    } catch (const json::exception&) {
        return error_response(400, "INVALID_JSON", "request body is not valid JSON");
    }
    if (!request.is_object()) {
        return error_response(400, "INVALID_REQUEST", "request body must be an object");
    }

    if (path == "/api/backup") {
        BackupRequest backup;
        if (!read_string(request, "source_path", backup.source_path) ||
            !read_string(request, "output_path", backup.output_path)) {
            return error_response(400, "INVALID_REQUEST", "source_path and output_path are required");
        }
        if (!read_filter_rules(request.value("filter_rules", json::object()), backup.filter_rules)) {
            return error_response(400, "INVALID_FILTER", "filter rules are invalid");
        }
        if (!is_directory(backup.source_path) || !is_readable(backup.source_path)) {
            return error_response(422, "INVALID_PATH", "source_path must be a readable directory");
        }
        const auto output = std::filesystem::path(backup.output_path);
        if (std::filesystem::exists(output)) {
            return error_response(409, "OUTPUT_EXISTS", "output archive already exists");
        }
        if (!is_writable_directory(output.parent_path()) ||
            is_inside(output, backup.source_path)) {
            return error_response(422, "INVALID_PATH", "output_path is not writable or is inside source_path");
        }
        const TaskSubmission submission = runtime_.submit_backup(backup);
        if (!submission.accepted()) {
            return error_response(429, "QUEUE_FULL", submission.result.message);
        }
        return json_response(202, {
            {"task_id", submission.task_id},
            {"type", "backup"},
            {"status", "PENDING"}
        });
    }

    RestoreRequest restore;
    std::string policy;
    if (!read_string(request, "archive_path", restore.archive_path) ||
        !read_string(request, "target_path", restore.target_path) ||
        !read_string(request, "conflict_policy", policy) ||
        !parse_conflict_policy(policy, restore.conflict_policy)) {
        return error_response(400, "INVALID_REQUEST", "archive_path, target_path and conflict_policy are required");
    }
    if (!is_regular_file(restore.archive_path) || !is_readable(restore.archive_path)) {
        return error_response(422, "INVALID_PATH", "archive_path must be a readable file");
    }
    const auto target_path = std::filesystem::path(restore.target_path);
    const auto parent = target_path.parent_path().empty() ? std::filesystem::path(".") : target_path.parent_path();
    if (!is_writable_directory(parent) || is_inside(restore.archive_path, target_path)) {
        return error_response(422, "INVALID_PATH", "target_path is not writable or contains archive_path");
    }
    const TaskSubmission submission = runtime_.submit_restore(restore);
    if (!submission.accepted()) {
        return error_response(429, "QUEUE_FULL", submission.result.message);
    }
    return json_response(202, {
        {"task_id", submission.task_id},
        {"type", "restore"},
        {"status", "PENDING"}
    });
}

void WebApi::mount(httplib::Server& server) {
    auto callback = [this](const httplib::Request& request, httplib::Response& response) {
        const ApiResponse result = handle(request.method, request.target, request.body);
        response.status = result.status;
        response.set_content(result.body, result.content_type);
        if (!config_.allowed_origin.empty()) {
            response.set_header("Access-Control-Allow-Origin", config_.allowed_origin);
        }
    };
    server.Get(R"(/api/.*)", callback);
    server.Post(R"(/api/.*)", callback);
    server.Options(R"(/api/.*)", [this](const httplib::Request&, httplib::Response& response) {
        response.status = 204;
        response.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        response.set_header("Access-Control-Allow-Headers", "Content-Type");
        if (!config_.allowed_origin.empty()) {
            response.set_header("Access-Control-Allow-Origin", config_.allowed_origin);
        }
    });
}

struct WebApiServer::Impl {
    Impl(TaskRuntime& runtime, ApiConfig config)
        : runtime(runtime)
        , config(std::move(config))
        , api(runtime, this->config) {}

    TaskRuntime& runtime;
    ApiConfig config;
    WebApi api;
    httplib::Server server;
    std::thread thread;
    int bound_port = 0;
    bool started = false;
};

WebApiServer::WebApiServer(TaskRuntime& runtime, ApiConfig config)
    : impl_(std::make_unique<Impl>(runtime, std::move(config))) {}

WebApiServer::~WebApiServer() {
    stop();
}

bool WebApiServer::start() {
    if (impl_->started) return true;
    impl_->runtime.start();
    impl_->api.mount(impl_->server);
    impl_->bound_port = impl_->server.bind_to_any_port(
        impl_->config.bind_address, impl_->config.port);
    if (impl_->bound_port <= 0) {
        impl_->runtime.shutdown();
        return false;
    }
    impl_->started = true;
    impl_->thread = std::thread([this] { impl_->server.listen_after_bind(); });
    return true;
}

void WebApiServer::stop() {
    if (!impl_->started) return;
    impl_->server.stop();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->started = false;
    impl_->runtime.shutdown();
}

int WebApiServer::port() const noexcept { return impl_->bound_port; }

}  // namespace backup
