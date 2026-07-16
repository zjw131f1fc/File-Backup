#include <gtest/gtest.h>
#include "web_api/web_api.h"
#include "../../tests/helpers/temp_dir.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

using namespace backup;
using namespace backup::testing;
using nlohmann::json;

namespace {

json response_json(const ApiResponse& response) {
    return json::parse(response.body);
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
    const std::string output = temp.path() + "/backup.dat";
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    runtime.start();
    WebApi api(runtime);

    const ApiResponse response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", output},
            {"filter_rules", json::object()}
        }.dump());
    const json body = response_json(response);

    EXPECT_EQ(response.status, 202);
    EXPECT_FALSE(body["task_id"].get<std::string>().empty());
    EXPECT_EQ(body["type"], "backup");
    EXPECT_EQ(body["status"], "PENDING");
    runtime.shutdown();
}

TEST(WebApiContract, ExistingOutputReturnsConflict) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    const std::string output = temp.create_file("backup.dat", "existing");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    WebApi api(runtime);

    const ApiResponse response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", output},
            {"filter_rules", json::object()}
        }.dump());

    EXPECT_EQ(response.status, 409);
    EXPECT_EQ(response_json(response)["error"]["code"], "OUTPUT_EXISTS");
}

TEST(WebApiContract, StoppedRuntimeReturnsServiceUnavailable) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    const std::string output = temp.path() + "/backup.dat";
    TaskManager task_manager;
    TaskRuntime runtime(task_manager);
    runtime.shutdown();
    WebApi api(runtime);

    const ApiResponse response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", output},
            {"filter_rules", json::object()}
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
            {"output_path", temp.path() + "/archive.dat"},
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
            {"output_path", temp.path() + "/archive.dat"},
            {"filter_rules", {{"include_types", {"NOT_A_TYPE"}}}}
        }.dump());

    EXPECT_EQ(response.status, 400);
    EXPECT_EQ(response_json(response)["error"]["code"], "INVALID_FILTER");
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
            {"output_path", source + "/archive.dat"},
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
        {"output_path", temp.path() + "/archive.dat"},
        {"filter_rules", json::object()}
    }.dump();

    ASSERT_EQ(api.handle("POST", "/api/backup", request).status, 202);
    const auto response = api.handle(
        "POST", "/api/backup", json{
            {"source_path", source},
            {"output_path", temp.path() + "/archive-2.dat"},
            {"filter_rules", json::object()}
        }.dump());
    EXPECT_EQ(response.status, 429);
    EXPECT_EQ(response_json(response)["error"]["code"], "QUEUE_FULL");
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
    server.stop();
}
