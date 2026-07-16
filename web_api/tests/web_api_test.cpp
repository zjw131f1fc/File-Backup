#include <gtest/gtest.h>
#include "web_api/web_api.h"
#include "../../tests/helpers/temp_dir.h"
#include "modules/archive_writer/archive_writer.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <thread>

using namespace backup;
using namespace backup::testing;
using nlohmann::json;

namespace {

json response_json(const ApiResponse& response) {
    return json::parse(response.body);
}

json wait_for_http_terminal(httplib::Client& client,
                            const std::string& task_id) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        const auto response = client.Get("/api/tasks/" + task_id);
        if (response && response->status == 200) {
            const auto body = json::parse(response->body);
            const std::string status = body.value("status", "");
            if (status == "SUCCESS" || status == "PARTIAL_SUCCESS" ||
                status == "FAILED" || status == "CANCELLED") {
                return body;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return json::object();
}

Task wait_for_terminal(TaskManager& task_manager, const std::string& task_id) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        const Task task = task_manager.get_task(task_id);
        if (task.status == TaskStatus::SUCCESS ||
            task.status == TaskStatus::PARTIAL_SUCCESS ||
            task.status == TaskStatus::FAILED ||
            task.status == TaskStatus::CANCELLED) {
            return task;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return task_manager.get_task(task_id);
}

}  // namespace

TEST(WebApiContract, HealthReturnsServiceStatus) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const ApiResponse response = api.handle("GET", "/api/health");

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response_json(response)["status"], "ok");
    EXPECT_EQ(response_json(response)["service"], "backup-web");
}

TEST(WebApiContract, CapabilitiesExposeRuntimeConfiguration) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 3, 9);
    WebApi api(runtime);

    const ApiResponse response = api.handle("GET", "/api/capabilities");
    const json body = response_json(response);

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(body["concurrency"]["worker_count"], 3);
    EXPECT_EQ(body["concurrency"]["max_queued_tasks"], 9);
    EXPECT_TRUE(body["progress_events"]);
}

TEST(WebApiContract, InvalidJsonReturnsBadRequest) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const ApiResponse response = api.handle("POST", "/api/backup", "{");
    const json body = response_json(response);

    EXPECT_EQ(response.status, 400);
    EXPECT_EQ(body["error"]["code"], "INVALID_JSON");
}

TEST(WebApiContract, BackupReturnsAcceptedTask) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    const std::string output_directory = temp.path();
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    runtime.start();
    WebApi api(runtime);

    const ApiResponse response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", output_directory},
            {"filter_rules", json::object()}
        }.dump());
    const json body = response_json(response);

    EXPECT_EQ(response.status, 202);
    EXPECT_FALSE(body["task_id"].get<std::string>().empty());
    EXPECT_EQ(body["type"], "backup");
    EXPECT_EQ(body["status"], "PENDING");
    runtime.shutdown();
}

TEST(WebApiContract, UsesDefaultArchiveNameAndRenamesConcurrentTasks) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    runtime.start();
    WebApi api(runtime);
    const auto request = [&temp, &source] {
        return json{
            {"source_path", source},
            {"output_path", temp.path()},
            {"filter_rules", json::object()}
        }.dump();
    };

    const auto first = api.handle("POST", "/api/backup", request());
    const auto second = api.handle("POST", "/api/backup", request());

    ASSERT_EQ(first.status, 202);
    ASSERT_EQ(second.status, 202);
    const auto first_id = response_json(first)["task_id"].get<std::string>();
    const auto second_id = response_json(second)["task_id"].get<std::string>();
    EXPECT_EQ(wait_for_terminal(task_manager, first_id).status, TaskStatus::SUCCESS);
    EXPECT_EQ(wait_for_terminal(task_manager, second_id).status, TaskStatus::SUCCESS);
    runtime.shutdown();
    EXPECT_TRUE(std::filesystem::exists(temp.path() + "/backup.dat"));
    EXPECT_TRUE(std::filesystem::exists(temp.path() + "/backup-1.dat"));
}

TEST(WebApiContract, UsesExplicitArchiveName) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 2);
    runtime.start();
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", temp.path()},
            {"archive_name", "nightly.dat"},
            {"filter_rules", json::object()}
        }.dump());

    ASSERT_EQ(response.status, 202);
    const auto task_id = response_json(response)["task_id"].get<std::string>();
    EXPECT_EQ(wait_for_terminal(task_manager, task_id).status, TaskStatus::SUCCESS);
    runtime.shutdown();
    EXPECT_TRUE(std::filesystem::exists(temp.path() + "/nightly.dat"));
}

TEST(WebApiContract, ExistingOutputReturnsConflict) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    const std::string output_directory = temp.path();
    temp.create_file("backup.dat", "existing");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const ApiResponse response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", output_directory},
            {"archive_name", "backup.dat"},
            {"filter_rules", json::object()}
        }.dump());

    EXPECT_EQ(response.status, 409);
    EXPECT_EQ(response_json(response)["error"]["code"], "OUTPUT_EXISTS");
}

TEST(WebApiContract, StoppedRuntimeReturnsServiceUnavailable) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    const std::string output_directory = temp.path();
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    runtime.shutdown();
    WebApi api(runtime);

    const ApiResponse response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", output_directory},
            {"filter_rules", json::object()}
        }.dump());

    EXPECT_EQ(response.status, 503);
    EXPECT_EQ(response_json(response)["error"]["code"], "RUNTIME_STOPPED");
}

TEST(WebApiContract, StoppedRuntimeRejectsRestore) {
    TempDir temp;
    const std::string archive = temp.create_file("archive.dat", "archive");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    runtime.shutdown();
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/restore", json{
            {"archive_path", archive},
            {"target_path", temp.path() + "/restore"},
            {"conflict_policy", "SKIP"}
        }.dump());

    EXPECT_EQ(response.status, 503);
    EXPECT_EQ(response_json(response)["error"]["code"], "RUNTIME_STOPPED");
}

TEST(WebApiServerContract, ServesHealthOverHttp) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    ApiConfig config;
    config.port = 0;
    WebApiServer server(runtime, config);

    ASSERT_TRUE(server.start());
    httplib::Client client("127.0.0.1", server.port());
    auto response = client.Get("/api/health");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body)["service"], "backup-web");
    server.stop();
}

TEST(WebApiServerContract, StreamsTaskEventsOverHttp) {
    TempDir temp;
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    ApiConfig config;
    config.port = 0;
    WebApiServer server(runtime, config);
    ASSERT_TRUE(server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto post = client.Post(
        "/api/backup",
        json{
            {"source_path", temp.create_dir("source")},
            {"output_path", temp.path()},
            {"filter_rules", json::object()}
        }.dump(),
        "application/json");
    ASSERT_TRUE(post);
    ASSERT_EQ(post->status, 202);
    const std::string task_id = json::parse(post->body)["task_id"];

    httplib::Client events_client("127.0.0.1", server.port());
    const auto events = events_client.Get("/api/tasks/" + task_id + "/events");
    ASSERT_TRUE(events) << "HTTP error: " << static_cast<int>(events.error());
    EXPECT_EQ(events->status, 200);
    EXPECT_NE(events->body.find("event: status"), std::string::npos);
    httplib::Headers invalid_cursor{{"Last-Event-ID", "not-an-integer"}};
    const auto events_with_invalid_cursor = events_client.Get(
        "/api/tasks/" + task_id + "/events", invalid_cursor);
    ASSERT_TRUE(events_with_invalid_cursor);
    EXPECT_EQ(events_with_invalid_cursor->status, 200);
    const auto missing_events = events_client.Get("/api/tasks/missing/events");
    ASSERT_TRUE(missing_events);
    EXPECT_EQ(missing_events->status, 404);
    server.stop();
}

TEST(WebApiServerContract, CompletesBackupOverHttp) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    temp.create_file("source/file.txt", "backup content");
    const std::string archive = temp.path() + "/backup.dat";

    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    ApiConfig config;
    config.port = 0;
    WebApiServer server(runtime, config);
    ASSERT_TRUE(server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto response = client.Post(
        "/api/backup",
        json{
            {"source_path", source},
            {"output_path", temp.path()},
            {"filter_rules", json::object()}
        }.dump(),
        "application/json");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 202);

    const std::string task_id = json::parse(response->body)["task_id"];
    const auto task = wait_for_http_terminal(client, task_id);
    ASSERT_FALSE(task.empty());
    EXPECT_EQ(task["status"], "SUCCESS");
    EXPECT_TRUE(std::filesystem::exists(archive));
    server.stop();
}

TEST(WebApiServerContract, CompletesRestoreOverHttp) {
    TempDir temp;
    const std::string input = temp.create_file("input.txt", "restore content");
    const std::string archive = temp.path() + "/backup.dat";
    const std::string target = temp.create_dir("restore");

    auto writer = create_archive(archive);
    ASSERT_NE(writer, nullptr);
    EntryInfo entry;
    entry.path = "restored.txt";
    entry.type = EntryType::REGULAR_FILE;
    entry.size = std::string("restore content").size();
    std::ifstream content(input, std::ios::binary);
    ASSERT_TRUE(content);
    ASSERT_EQ(writer->add_entry(entry, content).status, Status::SUCCESS);
    ASSERT_EQ(writer->commit().status, Status::SUCCESS);

    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    ApiConfig config;
    config.port = 0;
    WebApiServer server(runtime, config);
    ASSERT_TRUE(server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto response = client.Post(
        "/api/restore",
        json{
            {"archive_path", archive},
            {"target_path", target},
            {"conflict_policy", "OVERWRITE"}
        }.dump(),
        "application/json");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 202);

    const std::string task_id = json::parse(response->body)["task_id"];
    const auto task = wait_for_http_terminal(client, task_id);
    ASSERT_FALSE(task.empty());
    EXPECT_EQ(task["status"], "SUCCESS");

    std::ifstream restored(target + "/restored.txt", std::ios::binary);
    ASSERT_TRUE(restored);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(restored), {}),
              "restore content");
    server.stop();
}

TEST(WebApiContract, ListsAndReadsTasks) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";
    const auto submission = runtime.submit_backup(request);
    ASSERT_TRUE(submission.accepted());
    WebApi api(runtime);

    const auto list_response = api.handle("GET", "/api/tasks");
    const json list_body = response_json(list_response);
    ASSERT_EQ(list_response.status, 200);
    ASSERT_EQ(list_body["tasks"].size(), 1u);
    EXPECT_EQ(list_body["tasks"][0]["task_id"], submission.task_id);
    EXPECT_EQ(list_body["tasks"][0]["type"], "backup");

    const auto detail_response = api.handle(
        "GET", "/api/tasks/" + submission.task_id);
    EXPECT_EQ(detail_response.status, 200);
    EXPECT_EQ(response_json(detail_response)["task_id"], submission.task_id);
}

TEST(WebApiContract, CancelsPendingTask) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";
    const auto submission = runtime.submit_backup(request);
    ASSERT_TRUE(submission.accepted());
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/tasks/" + submission.task_id + "/cancel", "{}");

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response_json(response)["status"], "CANCELLED");
}

TEST(WebApiContract, ListsOnlyAllowedFilesystemEntries) {
    TempDir temp;
    temp.create_file("data.txt", "data");
    temp.create_dir("nested");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    ApiConfig config;
    config.allowed_roots = {temp.path()};
    WebApi api(runtime, config);

    const auto response = api.handle(
        "GET", "/api/filesystem/entries?path=" + temp.path());
    const json body = response_json(response);

    ASSERT_EQ(response.status, 200);
    EXPECT_EQ(body["path"], temp.path());
    ASSERT_EQ(body["entries"].size(), 2u);
}

TEST(WebApiContract, RejectsFilesystemPathOutsideAllowedRoots) {
    TempDir temp;
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    ApiConfig config;
    config.allowed_roots = {temp.path()};
    WebApi api(runtime, config);

    const auto response = api.handle("GET", "/api/filesystem/entries?path=/tmp");

    EXPECT_EQ(response.status, 403);
    EXPECT_EQ(response_json(response)["error"]["code"], "PATH_NOT_ALLOWED");
}

TEST(WebApiContract, FilesystemEntriesAreUnrestrictedWithoutConfiguredRoots) {
    TempDir temp;
    temp.create_file("visible.txt", "data");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    ApiConfig config;
    config.allowed_roots.clear();
    WebApi api(runtime, config);

    const auto response = api.handle(
        "GET", "/api/filesystem/entries?path=" + temp.path());

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response_json(response)["entries"].size(), 1u);
}

TEST(WebApiContract, ReturnsTaskEventsAsSse) {
    TempDir temp;
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    runtime.start();
    WebApi api(runtime);
    BackupRequest request;
    request.source_path = temp.create_dir("source");
    request.output_path = temp.path() + "/archive.dat";
    const auto submission = runtime.submit_backup(request);
    ASSERT_TRUE(submission.accepted());

    for (int attempt = 0; attempt < 100; ++attempt) {
        if (task_manager.get_task(submission.task_id).status == TaskStatus::SUCCESS) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto response = api.handle(
        "GET", "/api/tasks/" + submission.task_id + "/events");

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.content_type, "text/event-stream; charset=utf-8");
    EXPECT_NE(response.body.find("event: status"), std::string::npos);
    runtime.shutdown();
}

TEST(WebApiContract, RejectsNonObjectRequest) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const auto response = api.handle("POST", "/api/backup", "[]");

    EXPECT_EQ(response.status, 400);
    EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_REQUEST");
}

TEST(WebApiContract, RejectsMalformedFilterRules) {
    TempDir temp;
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", temp.create_dir("source")},
            {"output_path", temp.path()},
            {"filter_rules", {{"include_types", {"NOT_A_TYPE"}}}}
        }.dump());

    EXPECT_EQ(response.status, 400);
    EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_FILTER");
}

TEST(WebApiContract, RejectsArchiveNameWithDirectoryPath) {
    TempDir temp;
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", temp.create_dir("source")},
            {"output_path", temp.path()},
            {"archive_name", "nested/archive.dat"},
            {"filter_rules", json::object()}
        }.dump());

    EXPECT_EQ(response.status, 400);
    EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_REQUEST");
}

TEST(WebApiContract, RejectsOutputInsideSource) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", source},
            {"filter_rules", json::object()}
        }.dump());

    EXPECT_EQ(response.status, 422);
    EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_PATH");
}

TEST(WebApiContract, RejectsRestoreWhenTargetEqualsArchive) {
    TempDir temp;
    const std::string archive = temp.create_file("archive.dat", "archive");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/restore", json{
            {"archive_path", archive},
            {"target_path", archive},
            {"conflict_policy", "SKIP"}
        }.dump());

    EXPECT_EQ(response.status, 422);
    EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_PATH");
}

TEST(WebApiContract, RejectsInvalidRestorePolicy) {
    TempDir temp;
    const std::string archive = temp.create_file("archive.dat", "archive");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/restore", json{
            {"archive_path", archive},
            {"target_path", temp.path() + "/restore"},
            {"conflict_policy", "MERGE"}
        }.dump());

    EXPECT_EQ(response.status, 400);
    EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_REQUEST");
}

TEST(WebApiContract, FilesystemRootsAndEncodedEntriesAreAvailable) {
    TempDir temp;
    const std::string directory = temp.create_dir("folder name");
    temp.create_file("folder name/file.txt", "data");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    ApiConfig config;
    config.allowed_roots = {temp.path()};
    WebApi api(runtime, config);

    const auto roots = api.handle("GET", "/api/filesystem/roots");
    EXPECT_EQ(roots.status, 200);
    EXPECT_EQ(response_json(roots)["roots"].size(), 1u);

    const auto entries = api.handle(
        "GET", "/api/filesystem/entries?path=" +
            directory.substr(0, directory.find("folder name")) + "folder%20name");
    EXPECT_EQ(entries.status, 200);
    ASSERT_EQ(response_json(entries)["entries"].size(), 1u);
    EXPECT_EQ(response_json(entries)["entries"][0]["name"], "file.txt");
}

TEST(WebApiContract, FilesystemEntriesRequireReadableDirectoryPath) {
    TempDir temp;
    const std::string file = temp.create_file("file.txt", "data");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    ApiConfig config;
    config.allowed_roots = {temp.path()};
    WebApi api(runtime, config);

    EXPECT_EQ(api.handle("GET", "/api/filesystem/entries").status, 400);
    const auto response = api.handle(
        "GET", "/api/filesystem/entries?path=" + file);
    EXPECT_EQ(response.status, 404);
    EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_PATH");
}

TEST(WebApiContract, TaskListRejectsInvalidLimits) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    for (const auto& limit : {"abc", "0", "101"}) {
        const auto response = api.handle("GET", "/api/tasks?limit=" + std::string(limit));
        EXPECT_EQ(response.status, 400);
        EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_REQUEST");
    }
}

TEST(WebApiContract, TaskListFiltersByTypeAndStatus) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    BackupRequest backup;
    backup.source_path = "/source";
    backup.output_path = "/backup";
    RestoreRequest restore;
    restore.archive_path = "/archive";
    restore.target_path = "/target";
    const auto backup_submission = runtime.submit_backup(backup);
    const auto restore_submission = runtime.submit_restore(restore);
    ASSERT_TRUE(backup_submission.accepted());
    ASSERT_TRUE(restore_submission.accepted());
    WebApi api(runtime);

    const auto response = api.handle("GET", "/api/tasks?type=restore&status=PENDING");
    const auto body = response_json(response);
    ASSERT_EQ(response.status, 200);
    ASSERT_EQ(body["tasks"].size(), 1u);
    EXPECT_EQ(body["tasks"][0]["task_id"], restore_submission.task_id);
}

TEST(WebApiContract, UnknownTasksAndMethodsReturnNotFound) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    EXPECT_EQ(api.handle("GET", "/api/missing").status, 404);
    EXPECT_EQ(api.handle("PUT", "/api/health").status, 404);
    EXPECT_EQ(api.handle("GET", "/api/tasks/missing").status, 404);
    EXPECT_EQ(api.handle("GET", "/api/tasks/missing/events").status, 404);
    EXPECT_EQ(api.handle("GET", "/api/tasks/missing/events?after=bad").status, 404);
    EXPECT_EQ(api.handle("POST", "/api/tasks/missing/cancel", "{}").status, 404);
}

TEST(WebApiContract, RejectsCancelForFinishedTask) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";
    const auto submission = runtime.submit_backup(request);
    ASSERT_TRUE(submission.accepted());
    Result result;
    result.status = Status::SUCCESS;
    task_manager.complete_task(submission.task_id, result);
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/tasks/" + submission.task_id + "/cancel", "{}");
    EXPECT_EQ(response.status, 409);
    EXPECT_EQ(response_json(response)["error"]["code"], "TASK_CONFLICT");
}

TEST(WebApiContract, ReportsQueueFull) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 1);
    WebApi api(runtime);
    const auto request = json{
        {"source_path", source},
        {"output_path", temp.path()},
        {"filter_rules", json::object()}
    }.dump();

    ASSERT_EQ(api.handle("POST", "/api/backup", request).status, 202);
    const auto response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", temp.path()},
            {"archive_name", "archive-2.dat"},
            {"filter_rules", json::object()}
        }.dump());
    EXPECT_EQ(response.status, 429);
    EXPECT_EQ(response_json(response)["error"]["code"], "QUEUE_FULL");
}

TEST(WebApiContract, AcceptsCompleteFilterRules) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", temp.path()},
            {"filter_rules", {
                {"include_paths", {"src/*"}},
                {"exclude_paths", {"src/tmp/*"}},
                {"include_names", {"*.txt"}},
                {"exclude_names", {"*.tmp"}},
                {"include_types", {"REGULAR_FILE", "DIRECTORY"}},
                {"include_uids", {0, 1000}},
                {"newer_than_sec", 1},
                {"older_than_sec", 2},
                {"min_size", 1},
                {"max_size", 100}
            }}
        }.dump());

    EXPECT_EQ(response.status, 202);
}

TEST(WebApiContract, RejectsInvalidFilterShapesAndRanges) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);
    const auto request = [&source, &temp](const json& rules) {
        return json{
            {"source_path", source},
            {"output_path", temp.path()},
            {"filter_rules", rules}
        }.dump();
    };

    for (const auto& rules : {
        json{{"include_paths", "src"}},
        json{{"include_paths", {1}}},
        json{{"include_uids", 1000}},
        json{{"include_types", "REGULAR_FILE"}},
        json{{"include_uids", {"1000"}}},
        json{{"newer_than_sec", "yesterday"}},
        json{{"newer_than_sec", -1}},
        json{{"newer_than_sec", 10}, {"older_than_sec", 10}},
        json{{"min_size", 10}, {"max_size", 1}}
    }) {
        const auto response = api.handle("POST", "/api/backup", request(rules));
        EXPECT_EQ(response.status, 400);
        EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_FILTER");
    }
}

TEST(WebApiContract, ReportsActiveOutputConflict) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    const std::string output_directory = temp.path();
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);
    const auto request = json{
        {"source_path", source},
        {"output_path", output_directory},
        {"archive_name", "archive.dat"},
        {"filter_rules", json::object()}
    }.dump();

    ASSERT_EQ(api.handle("POST", "/api/backup", request).status, 202);
    const auto response = api.handle("POST", "/api/backup", request);

    EXPECT_EQ(response.status, 409);
    EXPECT_EQ(response_json(response)["error"]["code"], "OUTPUT_CONFLICT");
}

TEST(WebApiContract, AcceptsRestoreRequestForReadableArchive) {
    TempDir temp;
    const std::string archive = temp.create_file("archive.dat", "archive");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const auto response = api.handle(
        "POST", "/api/restore", json{
            {"archive_path", archive},
            {"target_path", temp.path() + "/restore"},
            {"conflict_policy", "OVERWRITE"}
        }.dump());

    EXPECT_EQ(response.status, 202);
    EXPECT_EQ(response_json(response)["type"], "restore");
}

TEST(WebApiContract, RejectsMissingBackupAndArchivePaths) {
    TempDir temp;
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const auto backup = api.handle(
        "POST", "/api/backup", json{
            {"source_path", temp.path()},
            {"filter_rules", json::object()}
        }.dump());
    EXPECT_EQ(backup.status, 400);

    const auto invalid_source = api.handle(
        "POST", "/api/backup", json{
            {"source_path", temp.path() + "/missing-source"},
            {"output_path", temp.path()},
            {"filter_rules", json::object()}
        }.dump());
    EXPECT_EQ(invalid_source.status, 422);

    const auto restore = api.handle(
        "POST", "/api/restore", json{
            {"target_path", temp.path()},
            {"conflict_policy", "SKIP"}
        }.dump());
    EXPECT_EQ(restore.status, 400);

    const auto invalid_archive = api.handle(
        "POST", "/api/restore", json{
            {"archive_path", temp.path() + "/missing.dat"},
            {"target_path", temp.path() + "/restore"},
            {"conflict_policy", "SKIP"}
        }.dump());
    EXPECT_EQ(invalid_archive.status, 422);
}

TEST(WebApiContract, SerializesAllTerminalTaskResults) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);
    std::vector<std::string> task_ids;
    for (int index = 0; index < 4; ++index) {
        BackupRequest request;
        request.source_path = "/source" + std::to_string(index);
        request.output_path = "/archive" + std::to_string(index);
        const auto submission = runtime.submit_backup(request);
        ASSERT_TRUE(submission.accepted());
        task_ids.push_back(submission.task_id);
    }

    Result success;
    success.status = Status::SUCCESS;
    task_manager.complete_task(task_ids[0], success);
    Result partial;
    partial.status = Status::PARTIAL_SUCCESS;
    task_manager.complete_task(task_ids[1], partial);
    ASSERT_TRUE(runtime.cancel_task(task_ids[2]));
    Result failed;
    failed.status = Status::FAILED;
    task_manager.complete_task(task_ids[3], failed);

    EXPECT_EQ(response_json(api.handle("GET", "/api/tasks"))["tasks"].size(), 4u);
    for (const auto& task_id : task_ids) {
        const auto response = api.handle("GET", "/api/tasks/" + task_id);
        EXPECT_EQ(response.status, 200);
        EXPECT_FALSE(response_json(response)["result"].is_null());
    }
}

TEST(WebApiContract, RejectsInvalidEventCursorForExistingTask) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";
    const auto submission = runtime.submit_backup(request);
    ASSERT_TRUE(submission.accepted());
    WebApi api(runtime);

    const auto response = api.handle(
        "GET", "/api/tasks/" + submission.task_id + "/events?after=bad");
    EXPECT_EQ(response.status, 400);
    EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_REQUEST");
}

TEST(WebApiServerContract, RejectsInvalidBindAddressAndAllowsRepeatedStop) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 2);
    ApiConfig config;
    config.port = -1;
    WebApiServer server(runtime, config);

    EXPECT_FALSE(server.start());
    server.stop();
    server.stop();
}

TEST(WebApiServerContract, OptionsExposeCorsHeaders) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    ApiConfig config;
    config.port = 0;
    config.allowed_origin = "http://localhost:3000";
    WebApiServer server(runtime, config);
    ASSERT_TRUE(server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto response = client.Options("/api/health");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 204);
    EXPECT_EQ(response->get_header_value("Access-Control-Allow-Origin"), config.allowed_origin);
    EXPECT_EQ(response->get_header_value("Access-Control-Allow-Methods"), "GET, POST, OPTIONS");
    const auto health = client.Get("/api/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->get_header_value("Access-Control-Allow-Origin"), config.allowed_origin);
    server.stop();
}
