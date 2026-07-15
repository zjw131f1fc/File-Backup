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
#include <sstream>
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

ApiResponse submission_error_response(const TaskSubmission& submission) {
    int status = 500;
    if (submission.error_code == "OUTPUT_CONFLICT") {
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

std::string url_decode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const auto hex = [](char character) -> int {
                if (character >= '0' && character <= '9') return character - '0';
                if (character >= 'a' && character <= 'f') return character - 'a' + 10;
                if (character >= 'A' && character <= 'F') return character - 'A' + 10;
                return -1;
            };
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
    result["created_at"] = snapshot.created_at;
    result["started_at"] = snapshot.started_at.empty()
        ? json(nullptr) : json(snapshot.started_at);
    result["finished_at"] = snapshot.finished_at.empty()
        ? json(nullptr) : json(snapshot.finished_at);
    return result;
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
    const std::string query = query_position == std::string::npos
        ? std::string() : target.substr(query_position + 1);

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

    if (method == "GET" && path == "/api/tasks") {
        std::string status_filter = query_value(query, "status");
        std::string type_filter = query_value(query, "type");
        std::size_t limit = 20;
        const std::string limit_value = query_value(query, "limit");
        if (!limit_value.empty()) {
            try {
                limit = std::stoul(limit_value);
            } catch (const std::exception&) {
                return error_response(400, "INVALID_REQUEST", "limit must be an integer");
            }
        }
        if (limit == 0 || limit > 100) {
            return error_response(400, "INVALID_REQUEST", "limit must be between 1 and 100");
        }
        json tasks = json::array();
        for (const auto& snapshot : runtime_.list_tasks()) {
            if ((!status_filter.empty() && task_status_name(snapshot.task.status) != status_filter) ||
                (!type_filter.empty() && snapshot.type != type_filter)) {
                continue;
            }
            tasks.push_back(snapshot_json(snapshot));
            if (tasks.size() >= limit) break;
        }
        return json_response(200, {{"tasks", tasks}});
    }

    if (method == "GET" && path == "/api/filesystem/roots") {
        json roots = json::array();
        for (const auto& root : config_.allowed_roots) {
            roots.push_back({
                {"path", normalized_path(root).string()},
                {"name", std::filesystem::path(root).filename().string()},
                {"type", "directory"}
            });
        }
        return json_response(200, {{"roots", roots}});
    }

    if (method == "GET" && path == "/api/filesystem/entries") {
        const std::string requested_path = query_value(query, "path");
        if (requested_path.empty()) {
            return error_response(400, "INVALID_REQUEST", "path is required");
        }
        if (!is_allowed_path(requested_path, config_.allowed_roots)) {
            return error_response(403, "PATH_NOT_ALLOWED", "path is outside allowed roots");
        }
        if (!is_directory(requested_path)) {
            return error_response(404, "INVALID_PATH", "path must be a directory");
        }
        std::error_code error;
        json entries = json::array();
        for (const auto& entry : std::filesystem::directory_iterator(requested_path, error)) {
            if (error) break;
            entries.push_back(filesystem_entry_json(entry));
        }
        if (error) {
            return error_response(422, "INVALID_PATH", "directory cannot be read");
        }
        std::sort(entries.begin(), entries.end(), [](const json& left, const json& right) {
            return left["name"].get<std::string>() < right["name"].get<std::string>();
        });
        return json_response(200, {{"path", normalized_path(requested_path).string()}, {"entries", entries}});
    }

    if (method == "GET" && path.rfind("/api/tasks/", 0) == 0) {
        const std::string suffix = path.substr(std::string("/api/tasks/").size());
        if (suffix.size() > 7 && suffix.compare(suffix.size() - 7, 7, "/events") == 0) {
            const std::string task_id = url_decode(suffix.substr(0, suffix.size() - 7));
            if (runtime_.get_task(task_id).task_id.empty()) {
                return error_response(404, "TASK_NOT_FOUND", "task not found");
            }
            std::size_t after_id = 0;
            const std::string after_value = query_value(query, "after");
            if (!after_value.empty()) {
                try { after_id = std::stoull(after_value); }
                catch (const std::exception&) {
                    return error_response(400, "INVALID_REQUEST", "after must be an integer");
                }
            }
            std::ostringstream stream;
            for (const auto& event : runtime_.get_events(task_id, after_id)) {
                stream << "event: " << event.type << "\n"
                       << "id: " << event.id << "\n"
                       << "data: " << task_json(event.task).dump() << "\n\n";
            }
            return {200, "text/event-stream; charset=utf-8", stream.str()};
        }
        const std::string task_id = url_decode(suffix);
        for (const auto& snapshot : runtime_.list_tasks()) {
            if (snapshot.task.task_id == task_id) {
                return json_response(200, snapshot_json(snapshot));
            }
        }
        return error_response(404, "TASK_NOT_FOUND", "task not found");
    }

    if (method == "POST" && path.rfind("/api/tasks/", 0) == 0 &&
        path.size() > 7 && path.compare(path.size() - 7, 7, "/cancel") == 0) {
        const std::string task_id = url_decode(path.substr(
            std::string("/api/tasks/").size(),
            path.size() - std::string("/api/tasks/").size() - 7));
        const Task before = runtime_.get_task(task_id);
        if (before.task_id.empty()) {
            return error_response(404, "TASK_NOT_FOUND", "task not found");
        }
        if (!runtime_.cancel_task(task_id)) {
            return error_response(409, "TASK_CONFLICT", "task cannot be cancelled in its current state");
        }
        return json_response(200, {{"task_id", task_id}, {"status", "CANCELLED"}});
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
            return submission_error_response(submission);
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
        return submission_error_response(submission);
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
    server.Get(R"(/api/tasks/([^/]+)/events)", [this](const httplib::Request& request,
                                                        httplib::Response& response) {
        const std::string task_id = url_decode(request.matches[1].str());
        if (runtime_.get_task(task_id).task_id.empty()) {
            const ApiResponse result = error_response(404, "TASK_NOT_FOUND", "task not found");
            response.status = result.status;
            response.set_content(result.body, result.content_type);
            return;
        }

        uint64_t after_id = 0;
        const std::string last_event_id = request.get_header_value("Last-Event-ID");
        if (!last_event_id.empty()) {
            try { after_id = std::stoull(last_event_id); }
            catch (const std::exception&) { after_id = 0; }
        }
        response.set_chunked_content_provider(
            "text/event-stream; charset=utf-8",
            [this, task_id, after_id](size_t, httplib::DataSink& sink) mutable {
                for (int attempt = 0; attempt < 600 && sink.is_writable(); ++attempt) {
                    for (const auto& event : runtime_.get_events(task_id, after_id)) {
                        std::ostringstream stream;
                        stream << "event: " << event.type << "\n"
                               << "id: " << event.id << "\n"
                               << "data: " << task_json(event.task).dump() << "\n\n";
                        const std::string payload = stream.str();
                        sink.write(payload.data(), payload.size());
                        after_id = event.id;
                    }
                    const auto task = runtime_.get_task(task_id);
                    const bool terminal = task.status == TaskStatus::SUCCESS ||
                        task.status == TaskStatus::PARTIAL_SUCCESS ||
                        task.status == TaskStatus::FAILED ||
                        task.status == TaskStatus::CANCELLED;
                    if (terminal && runtime_.get_events(task_id, after_id).empty()) {
                        sink.done();
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                sink.done();
                return true;
            });
    });
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
    if (impl_->config.port == 0) {
        impl_->bound_port = impl_->server.bind_to_any_port(impl_->config.bind_address);
    } else if (impl_->server.bind_to_port(impl_->config.bind_address, impl_->config.port)) {
        impl_->bound_port = impl_->config.port;
    } else {
        impl_->bound_port = 0;
    }
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
