#include "web_api/web_api.h"
#include "web_api_internal.h"
#include <httplib.h>
#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <utility>

namespace backup {

using namespace web_api_internal;

WebApi::WebApi(TaskRuntime& runtime, ApiConfig config)
    : runtime_(runtime), config_(std::move(config)) {}

// Parse the target and dispatch requests to the API's small set of routes.
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
        const std::string status_filter = query_value(query, "status");
        const std::string type_filter = query_value(query, "type");
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
        if (!config_.allowed_roots.empty() &&
            !is_allowed_path(requested_path, config_.allowed_roots)) {
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
        return json_response(200, {
            {"path", normalized_path(requested_path).string()},
            {"entries", entries}
        });
    }

    if (method == "POST" && path == "/api/filesystem/directories") {
        json request;
        try {
            request = json::parse(body);
        } catch (const json::exception&) {
            return error_response(400, "INVALID_JSON", "request body is not valid JSON");
        }
        if (!request.is_object()) {
            return error_response(400, "INVALID_REQUEST", "request body must be an object");
        }

        std::string parent_path;
        std::string name;
        if (!read_string(request, "parent_path", parent_path) ||
            !read_string(request, "name", name) ||
            !valid_directory_name(name)) {
            return error_response(400, "INVALID_REQUEST",
                                  "parent_path and a single directory name are required");
        }
        if (!config_.allowed_roots.empty() &&
            !is_allowed_path(parent_path, config_.allowed_roots)) {
            return error_response(403, "PATH_NOT_ALLOWED", "path is outside allowed roots");
        }
        if (!is_directory(parent_path) || !is_writable_directory(parent_path)) {
            return error_response(422, "INVALID_PATH",
                                  "parent_path must be a writable directory");
        }

        const auto target = normalized_path(std::filesystem::path(parent_path) / name);
        std::error_code status_error;
        const auto target_status = std::filesystem::symlink_status(target, status_error);
        if (!status_error &&
            target_status.type() != std::filesystem::file_type::not_found) {
            return error_response(409, "DIRECTORY_EXISTS", "directory already exists");
        }

        std::error_code create_error;
        if (!std::filesystem::create_directory(target, create_error)) {
            return error_response(422, "DIRECTORY_CREATE_FAILED",
                                  create_error ? create_error.message()
                                               : "directory could not be created");
        }
        return json_response(201, {
            {"path", target.string()},
            {"name", target.filename().string()},
            {"type", "directory"}
        });
    }

    if (method == "GET" && path.rfind("/api/tasks/", 0) == 0) {
        const std::string prefix = "/api/tasks/";
        const std::string suffix = path.substr(prefix.size());
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
        const std::string prefix = "/api/tasks/";
        const std::string task_id = url_decode(path.substr(
            prefix.size(), path.size() - prefix.size() - 7));
        if (runtime_.get_task(task_id).task_id.empty()) {
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
    return handle_task_submission(path, body);
}

// Validate and submit one backup or restore request.
ApiResponse WebApi::handle_task_submission(const std::string& path,
                                           const std::string& body) {
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
        if (request.contains("archive_name")) {
            if (!request["archive_name"].is_string()) {
                return error_response(400, "INVALID_REQUEST", "archive_name must be a file name");
            }
            backup.archive_name = request["archive_name"].get<std::string>();
            if (!valid_archive_name(backup.archive_name)) {
                return error_response(400, "INVALID_REQUEST", "archive_name must not contain a directory path");
            }
        }
        if (!read_filter_rules(request.value("filter_rules", json::object()), backup.filter_rules)) {
            return error_response(400, "INVALID_FILTER", "filter rules are invalid");
        }
        if (!is_directory(backup.source_path) || !is_readable(backup.source_path)) {
            return error_response(422, "INVALID_PATH", "source_path must be a readable directory");
        }
        const auto output_directory = std::filesystem::path(backup.output_path);
        if (!is_writable_directory(output_directory) ||
            is_same_or_inside(output_directory, backup.source_path)) {
            return error_response(422, "INVALID_PATH", "output_path must be a writable directory outside source_path");
        }
        backup.output_directory = backup.output_path;
        backup.output_path.clear();
        const TaskSubmission submission = runtime_.submit_backup(backup);
        if (!submission.accepted()) return submission_error_response(submission);
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
    const auto parent = target_path.parent_path().empty()
        ? std::filesystem::path(".") : target_path.parent_path();
    if (!is_writable_directory(parent) ||
        is_same_or_inside(restore.archive_path, target_path)) {
        return error_response(422, "INVALID_PATH", "target_path is not writable or contains archive_path");
    }
    const TaskSubmission submission = runtime_.submit_restore(restore);
    if (!submission.accepted()) return submission_error_response(submission);
    return json_response(202, {
        {"task_id", submission.task_id},
        {"type", "restore"},
        {"status", "PENDING"}
    });
}

// Register normal HTTP routes and the optional SSE progress stream.
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
                    if (is_terminal_status(task.status) &&
                        runtime_.get_events(task_id, after_id).empty()) {
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

}  // namespace backup
