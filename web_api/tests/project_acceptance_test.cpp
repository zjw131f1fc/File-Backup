#include <gtest/gtest.h>
#include "web_api/web_api.h"
#include "modules/archive_reader/archive_reader.h"
#include "modules/archive_writer/archive_writer.h"
#include "modules/filter/filter.h"
#include "modules/scanner/scanner.h"
#include "../../tests/helpers/filesystem_fixture.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sys/sysmacros.h>
#include <thread>

using namespace backup;
using namespace backup::testing;
using nlohmann::json;

namespace {

bool is_terminal(const std::string& status) {
    return status == "SUCCESS" || status == "PARTIAL_SUCCESS" ||
           status == "FAILED" || status == "CANCELLED";
}

json response_body(const httplib::Result& response) {
    EXPECT_TRUE(response);
    if (!response) return json::object();
    return json::parse(response->body);
}

json wait_for_terminal(httplib::Client& client,
                       const std::string& task_id,
                       std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto response = client.Get("/api/tasks/" + task_id);
        if (response && response->status == 200) {
            const auto body = json::parse(response->body);
            if (is_terminal(body.value("status", ""))) return body;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return json::object();
}

std::string url_encode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.' ||
            character == '~') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hex[character >> 4]);
            result.push_back(hex[character & 0x0f]);
        }
    }
    return result;
}

std::vector<EntryInfo> archive_entries(const std::string& path) {
    std::vector<EntryInfo> entries;
    auto reader = open_archive(path);
    if (!reader || !reader->validate().ok()) return entries;
    while (reader->has_next_entry()) {
        EntryInfo entry;
        if (!reader->next_entry(entry).ok()) return {};
        entries.push_back(entry);
    }
    return entries;
}

bool has_entry(const std::vector<EntryInfo>& entries,
               const std::string& path,
               EntryType type) {
    return std::any_of(entries.begin(), entries.end(), [&](const EntryInfo& entry) {
        return entry.path == path && entry.type == type;
    });
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class HttpHarness {
public:
    explicit HttpHarness(std::size_t workers = 2,
                         std::size_t queue_size = 16,
                         bool restrict_roots = true)
        : runtime(task_manager, workers, queue_size)
        , server(runtime, make_config(restrict_roots)) {}

    bool start() { return server.start(); }

    httplib::Client client() const {
        httplib::Client result("127.0.0.1", server.port());
        result.set_connection_timeout(1, 0);
        result.set_read_timeout(10, 0);
        return result;
    }

    TempDir temp;

private:
    TaskManager task_manager;
    TaskRuntime runtime;
    WebApiServer server;

    ApiConfig make_config(bool restrict_roots) const {
        ApiConfig config;
        config.port = 0;
        config.allowed_origin = "http://127.0.0.1:4173";
        if (restrict_roots) config.allowed_roots = {temp.path()};
        return config;
    }
};

json backup_request(const std::string& source,
                    const std::string& output,
                    const std::string& archive_name = "",
                    const json& filter_rules = json::object()) {
    json request = {
        {"source_path", source},
        {"output_path", output},
        {"filter_rules", filter_rules}
    };
    if (!archive_name.empty()) request["archive_name"] = archive_name;
    return request;
}

json restore_request(const std::string& archive,
                     const std::string& target,
                     const std::string& policy) {
    return {
        {"archive_path", archive},
        {"target_path", target},
        {"conflict_policy", policy}
    };
}

struct BlockingState {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
};

class BlockingScanner final : public IScanner {
public:
    explicit BlockingScanner(std::shared_ptr<BlockingState> state)
        : state_(std::move(state)) {}

    Result scan_and_backup(const std::string&,
                           IFilter&,
                           IArchiveWriter&,
                           ProgressCallback) override {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->entered = true;
        }
        state_->condition.notify_all();
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->condition.wait(lock, [this] { return state_->release; });
        Result result;
        result.status = Status::SUCCESS;
        return result;
    }

private:
    std::shared_ptr<BlockingState> state_;
};

}  // namespace

TEST(ProjectAcceptance, HealthCapabilitiesFilesystemAndCorsWorkOverHttp) {
    HttpHarness harness;
    ASSERT_TRUE(harness.start());
    auto client = harness.client();
    const auto health = client.Get("/api/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    EXPECT_EQ(response_body(health)["status"], "ok");
    EXPECT_EQ(health->get_header_value("Access-Control-Allow-Origin"),
              "http://127.0.0.1:4173");

    const auto capabilities = client.Get("/api/capabilities");
    ASSERT_TRUE(capabilities);
    const auto capability_body = response_body(capabilities);
    EXPECT_EQ(capabilities->status, 200);
    EXPECT_EQ(capability_body["entry_types"].size(), 7u);
    EXPECT_TRUE(capability_body["concurrency"]["enabled"]);
    EXPECT_TRUE(capability_body["progress_events"]);

    harness.temp.create_file("folder name/file.txt", "visible");
    const auto roots = client.Get("/api/filesystem/roots");
    ASSERT_TRUE(roots);
    EXPECT_EQ(roots->status, 200);
    EXPECT_EQ(response_body(roots)["roots"].size(), 1u);
    const auto entries = client.Get(
        "/api/filesystem/entries?path=" + url_encode(harness.temp.path()));
    ASSERT_TRUE(entries);
    EXPECT_EQ(entries->status, 200);
    EXPECT_NE(response_body(entries).dump().find("folder name"), std::string::npos);
    const auto outside = client.Get("/api/filesystem/entries?path=/tmp");
    ASSERT_TRUE(outside);
    EXPECT_EQ(outside->status, 403);
    EXPECT_EQ(response_body(outside)["error"]["code"], "PATH_NOT_ALLOWED");

    const auto options = client.Options("/api/health");
    ASSERT_TRUE(options);
    EXPECT_EQ(options->status, 204);
    EXPECT_EQ(options->get_header_value("Access-Control-Allow-Methods"),
              "GET, POST, OPTIONS");
}

TEST(ProjectAcceptance, CreatesAndListsDirectoriesOverHttp) {
    HttpHarness harness;
    ASSERT_TRUE(harness.start());
    auto client = harness.client();
    const auto create = client.Post(
        "/api/filesystem/directories",
        json{{"parent_path", harness.temp.path()}, {"name", "new-folder"}}.dump(),
        "application/json");
    ASSERT_TRUE(create);
    ASSERT_EQ(create->status, 201);
    const auto created_body = response_body(create);
    const std::string created_path = created_body["path"];
    EXPECT_EQ(created_body["type"], "directory");
    EXPECT_TRUE(std::filesystem::is_directory(created_path));

    const auto entries = client.Get(
        "/api/filesystem/entries?path=" + url_encode(harness.temp.path()));
    ASSERT_TRUE(entries);
    EXPECT_NE(response_body(entries).dump().find("new-folder"), std::string::npos);

    const auto duplicate = client.Post(
        "/api/filesystem/directories",
        json{{"parent_path", harness.temp.path()}, {"name", "new-folder"}}.dump(),
        "application/json");
    ASSERT_TRUE(duplicate);
    EXPECT_EQ(duplicate->status, 409);
    EXPECT_EQ(response_body(duplicate)["error"]["code"], "DIRECTORY_EXISTS");

    const auto invalid_name = client.Post(
        "/api/filesystem/directories",
        json{{"parent_path", harness.temp.path()}, {"name", "nested/folder"}}.dump(),
        "application/json");
    ASSERT_TRUE(invalid_name);
    EXPECT_EQ(invalid_name->status, 400);
    EXPECT_EQ(response_body(invalid_name)["error"]["code"], "INVALID_REQUEST");
}

TEST(ProjectAcceptance, BacksUpAndRestoresFilesLinksFifoAndMetadataOverHttp) {
    HttpHarness harness;
    ASSERT_TRUE(harness.start());
    auto client = harness.client();
    const std::string source = harness.temp.create_dir("source");
    const std::string output = harness.temp.create_dir("output");
    const std::string target = harness.temp.create_dir("restore");

    harness.temp.create_file("source/regular.txt", "regular content");
    harness.temp.create_file("source/nested/large.bin", std::string(128 * 1024, 'x'));
    harness.temp.create_dir("source/empty");
    harness.temp.create_hardlink("source/hard.txt", "source/regular.txt");
    harness.temp.create_symlink("source/link.txt", "regular.txt");
    harness.temp.create_fifo("source/pipe");
    ASSERT_TRUE(create_unix_socket(std::filesystem::path(source) / "socket"));
    ASSERT_EQ(::chmod((std::filesystem::path(source) / "regular.txt").c_str(), 0640), 0);
    const timespec access_time{1700000000, 123456789};
    const timespec modified_time{1700000100, 987654321};
    ASSERT_TRUE(set_file_times(std::filesystem::path(source) / "regular.txt",
                               access_time, modified_time));

    const auto backup = client.Post(
        "/api/backup",
        backup_request(source, output, "full.dat").dump(),
        "application/json");
    ASSERT_TRUE(backup);
    ASSERT_EQ(backup->status, 202);
    const std::string backup_id = response_body(backup)["task_id"];
    const auto backup_task = wait_for_terminal(client, backup_id);
    ASSERT_FALSE(backup_task.empty());
    EXPECT_EQ(backup_task["status"], "SUCCESS");
    const std::string archive = output + "/full.dat";
    ASSERT_TRUE(std::filesystem::exists(archive));
    EXPECT_FALSE(std::filesystem::exists(archive + ".tmp"));

    const auto entries = archive_entries(archive);
    EXPECT_TRUE(has_entry(entries, "regular.txt", EntryType::REGULAR_FILE));
    EXPECT_TRUE(has_entry(entries, "hard.txt", EntryType::HARD_LINK));
    EXPECT_TRUE(has_entry(entries, "link.txt", EntryType::SYMBOLIC_LINK));
    EXPECT_TRUE(has_entry(entries, "pipe", EntryType::FIFO));
    EXPECT_TRUE(has_entry(entries, "empty", EntryType::DIRECTORY));
    EXPECT_FALSE(std::any_of(entries.begin(), entries.end(), [](const EntryInfo& entry) {
        return entry.path == "socket";
    }));

    const auto restore = client.Post(
        "/api/restore",
        restore_request(archive, target, "OVERWRITE").dump(),
        "application/json");
    ASSERT_TRUE(restore);
    ASSERT_EQ(restore->status, 202);
    const std::string restore_id = response_body(restore)["task_id"];
    const auto restore_task = wait_for_terminal(client, restore_id);
    ASSERT_FALSE(restore_task.empty());
    EXPECT_EQ(restore_task["status"], "SUCCESS");

    EXPECT_EQ(read_file(std::filesystem::path(target) / "regular.txt"),
              "regular content");
    struct stat regular_stat{};
    struct stat hard_stat{};
    ASSERT_EQ(::lstat((std::filesystem::path(target) / "regular.txt").c_str(),
                      &regular_stat), 0);
    ASSERT_EQ(::lstat((std::filesystem::path(target) / "hard.txt").c_str(),
                      &hard_stat), 0);
    EXPECT_EQ(regular_stat.st_ino, hard_stat.st_ino);
    EXPECT_EQ(std::filesystem::read_symlink(
                  std::filesystem::path(target) / "link.txt").string(),
              "regular.txt");
    struct stat fifo_stat{};
    ASSERT_EQ(::lstat((std::filesystem::path(target) / "pipe").c_str(), &fifo_stat), 0);
    EXPECT_TRUE(S_ISFIFO(fifo_stat.st_mode));
    EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::path(target) / "empty"));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(target) / "socket"));
    EXPECT_EQ(regular_stat.st_mode & 07777, 0640);
    EXPECT_EQ(regular_stat.st_mtime, modified_time.tv_sec);
}

TEST(ProjectAcceptance, AppliesAllSixFiltersThroughHttp) {
    HttpHarness harness;
    ASSERT_TRUE(harness.start());
    auto client = harness.client();
    const std::string source = harness.temp.create_dir("source");
    const std::string output = harness.temp.create_dir("output");
    harness.temp.create_file("source/included/keep.txt", "123456789");
    harness.temp.create_file("source/included/skip.txt", "123456789");
    harness.temp.create_file("source/included/small.txt", "x");
    harness.temp.create_file("source/included/blocked/keep.txt", "123456789");
    harness.temp.create_file("source/other/keep.txt", "123456789");
    const timespec access_time{1700000000, 0};
    const timespec modified_time{1700000100, 0};
    ASSERT_TRUE(set_file_times(
        std::filesystem::path(source) / "included/keep.txt",
        access_time, modified_time));

    const json filters = {
        {"include_paths", {"included/"}},
        {"exclude_paths", {"included/blocked/"}},
        {"include_names", {"*.txt"}},
        {"exclude_names", {"skip*"}},
        {"newer_than_sec", 1600000000},
        {"older_than_sec", 1800000000},
        {"min_size", 5},
        {"max_size", 20},
        {"include_types", {"REGULAR_FILE"}},
        {"include_uids", {static_cast<unsigned int>(::getuid())}}
    };
    const auto response = client.Post(
        "/api/backup", backup_request(source, output, "filtered.dat", filters).dump(),
        "application/json");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 202);
    const auto task = wait_for_terminal(client, response_body(response)["task_id"]);
    ASSERT_FALSE(task.empty());
    EXPECT_EQ(task["status"], "SUCCESS");

    const auto entries = archive_entries(output + "/filtered.dat");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(has_entry(entries, "included/keep.txt", EntryType::REGULAR_FILE));
}

TEST(ProjectAcceptance, AllocatesDefaultNamesAndRejectsExplicitConflictsOverHttp) {
    HttpHarness harness;
    ASSERT_TRUE(harness.start());
    auto client = harness.client();
    const std::string source = harness.temp.create_dir("source");
    const std::string output = harness.temp.create_dir("output");
    harness.temp.create_file("source/file.txt", "data");

    const auto first = client.Post(
        "/api/backup", backup_request(source, output).dump(), "application/json");
    const auto second = client.Post(
        "/api/backup", backup_request(source, output).dump(), "application/json");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_EQ(first->status, 202);
    ASSERT_EQ(second->status, 202);
    EXPECT_EQ(wait_for_terminal(client, response_body(first)["task_id"])["status"], "SUCCESS");
    EXPECT_EQ(wait_for_terminal(client, response_body(second)["task_id"])["status"], "SUCCESS");
    EXPECT_TRUE(std::filesystem::exists(output + "/backup.dat"));
    EXPECT_TRUE(std::filesystem::exists(output + "/backup-1.dat"));

    const auto named = client.Post(
        "/api/backup", backup_request(source, output, "named.dat").dump(),
        "application/json");
    ASSERT_TRUE(named);
    ASSERT_EQ(named->status, 202);
    EXPECT_EQ(wait_for_terminal(client, response_body(named)["task_id"])["status"], "SUCCESS");
    const auto conflict = client.Post(
        "/api/backup", backup_request(source, output, "named.dat").dump(),
        "application/json");
    ASSERT_TRUE(conflict);
    EXPECT_EQ(conflict->status, 409);
    EXPECT_EQ(response_body(conflict)["error"]["code"], "OUTPUT_EXISTS");
}

TEST(ProjectAcceptance, SupportsAllRestoreConflictPoliciesOverHttp) {
    HttpHarness harness;
    ASSERT_TRUE(harness.start());
    auto client = harness.client();
    const std::string source = harness.temp.create_dir("source");
    const std::string output = harness.temp.create_dir("output");
    const std::string target = harness.temp.create_dir("target");
    harness.temp.create_file("source/file.txt", "archived");
    const auto backup = client.Post(
        "/api/backup", backup_request(source, output, "conflict.dat").dump(),
        "application/json");
    ASSERT_TRUE(backup);
    ASSERT_EQ(backup->status, 202);
    ASSERT_EQ(wait_for_terminal(client, response_body(backup)["task_id"])["status"], "SUCCESS");
    const std::string archive = output + "/conflict.dat";

    harness.temp.create_file("target/file.txt", "existing");
    auto skip = client.Post(
        "/api/restore", restore_request(archive, target, "SKIP").dump(),
        "application/json");
    ASSERT_TRUE(skip);
    ASSERT_EQ(skip->status, 202);
    EXPECT_EQ(wait_for_terminal(client, response_body(skip)["task_id"])["status"], "SUCCESS");
    EXPECT_EQ(read_file(std::filesystem::path(target) / "file.txt"), "existing");

    auto overwrite = client.Post(
        "/api/restore", restore_request(archive, target, "OVERWRITE").dump(),
        "application/json");
    ASSERT_TRUE(overwrite);
    ASSERT_EQ(overwrite->status, 202);
    EXPECT_EQ(wait_for_terminal(client, response_body(overwrite)["task_id"])["status"], "SUCCESS");
    EXPECT_EQ(read_file(std::filesystem::path(target) / "file.txt"), "archived");

    harness.temp.create_file("target/file.txt", "existing again");
    auto rename = client.Post(
        "/api/restore", restore_request(archive, target, "RENAME").dump(),
        "application/json");
    ASSERT_TRUE(rename);
    ASSERT_EQ(rename->status, 202);
    EXPECT_EQ(wait_for_terminal(client, response_body(rename)["task_id"])["status"], "SUCCESS");
    EXPECT_EQ(read_file(std::filesystem::path(target) / "file.txt.1"), "archived");
}

TEST(ProjectAcceptance, RunsConcurrentBackupAndRestoreTasksOverHttp) {
    HttpHarness harness(2, 8);
    ASSERT_TRUE(harness.start());
    auto client = harness.client();
    const std::string output = harness.temp.create_dir("output");
    std::vector<std::string> sources;
    for (int index = 0; index < 4; ++index) {
        const std::string source = harness.temp.create_dir("source-" + std::to_string(index));
        harness.temp.create_file("source-" + std::to_string(index) + "/file.txt",
                                 "data-" + std::to_string(index));
        sources.push_back(source);
    }

    std::vector<std::string> backup_ids;
    for (int index = 0; index < 4; ++index) {
        const auto response = client.Post(
            "/api/backup",
            backup_request(sources[index], output, "parallel-" + std::to_string(index) + ".dat").dump(),
            "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 202);
        backup_ids.push_back(response_body(response)["task_id"]);
    }
    for (const auto& task_id : backup_ids) {
        EXPECT_EQ(wait_for_terminal(client, task_id)["status"], "SUCCESS");
    }

    std::vector<std::string> restore_ids;
    for (int index = 0; index < 4; ++index) {
        const std::string target = harness.temp.create_dir("restore-" + std::to_string(index));
        const auto response = client.Post(
            "/api/restore",
            restore_request(output + "/parallel-" + std::to_string(index) + ".dat",
                            target, "OVERWRITE").dump(),
            "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 202);
        restore_ids.push_back(response_body(response)["task_id"]);
    }
    for (const auto& task_id : restore_ids) {
        EXPECT_EQ(wait_for_terminal(client, task_id)["status"], "SUCCESS");
    }
    const auto list = client.Get("/api/tasks?status=SUCCESS&limit=20");
    ASSERT_TRUE(list);
    EXPECT_EQ(response_body(list)["tasks"].size(), 8u);
}

TEST(ProjectAcceptance, CancelsQueuedTaskOverHttp) {
    TempDir temp;
    const std::string source = temp.create_dir("source");
    const std::string output = temp.create_dir("output");
    auto state = std::make_shared<BlockingState>();
    TaskRuntimeFactories factories;
    factories.scanner = [state] { return std::make_unique<BlockingScanner>(state); };
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4, std::move(factories));
    ApiConfig config;
    config.port = 0;
    config.allowed_roots = {temp.path()};
    WebApiServer server(runtime, config);
    ASSERT_TRUE(server.start());
    httplib::Client client("127.0.0.1", server.port());

    const auto first = client.Post(
        "/api/backup", backup_request(source, output, "blocked.dat").dump(),
        "application/json");
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, 202);
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        ASSERT_TRUE(state->condition.wait_for(lock, std::chrono::seconds(1),
            [&] { return state->entered; }));
    }

    const auto second = client.Post(
        "/api/backup", backup_request(source, output, "queued.dat").dump(),
        "application/json");
    ASSERT_TRUE(second);
    ASSERT_EQ(second->status, 202);
    const std::string second_id = response_body(second)["task_id"];
    const auto cancel = client.Post(
        "/api/tasks/" + second_id + "/cancel", "{}", "application/json");
    ASSERT_TRUE(cancel);
    EXPECT_EQ(cancel->status, 200);
    EXPECT_EQ(response_body(cancel)["status"], "CANCELLED");

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
    }
    state->condition.notify_all();
    EXPECT_EQ(wait_for_terminal(client, response_body(first)["task_id"])["status"], "SUCCESS");
    EXPECT_EQ(wait_for_terminal(client, second_id)["status"], "CANCELLED");
    server.stop();
}

TEST(ProjectAcceptance, ReportsStableErrorsOverHttp) {
    HttpHarness harness;
    ASSERT_TRUE(harness.start());
    auto client = harness.client();
    const std::string source = harness.temp.create_dir("source");
    const std::string output = harness.temp.create_dir("output");

    struct ErrorCase {
        std::string body;
        int status;
        const char* code;
    } cases[] = {
        {"{", 400, "INVALID_JSON"},
        {"[]", 400, "INVALID_REQUEST"},
        {"{\"source_path\":\"x\"}", 400, "INVALID_REQUEST"},
        {backup_request(source, output, "nested/file.dat").dump(), 400, "INVALID_REQUEST"},
        {json{{"source_path", source}, {"output_path", output},
              {"filter_rules", {{"include_types", {"BAD"}}}}}.dump(),
         400, "INVALID_FILTER"}
    };
    for (const auto& test_case : cases) {
        const auto response = client.Post("/api/backup", test_case.body,
                                          "application/json");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, test_case.status);
        EXPECT_EQ(response_body(response)["error"]["code"], test_case.code);
    }

    const auto missing_task = client.Get("/api/tasks/missing");
    ASSERT_TRUE(missing_task);
    EXPECT_EQ(missing_task->status, 404);
    EXPECT_EQ(response_body(missing_task)["error"]["code"], "TASK_NOT_FOUND");
    const auto invalid_restore = client.Post(
        "/api/restore", restore_request(output + "/missing.dat", output, "SKIP").dump(),
        "application/json");
    ASSERT_TRUE(invalid_restore);
    EXPECT_EQ(invalid_restore->status, 422);
    EXPECT_EQ(response_body(invalid_restore)["error"]["code"], "INVALID_PATH");

    const std::string corrupt_archive = harness.temp.create_file(
        "output/corrupt.dat", "not a backup archive");
    const auto failed_restore = client.Post(
        "/api/restore", restore_request(corrupt_archive,
                                         output + "/error-target", "SKIP").dump(),
        "application/json");
    ASSERT_TRUE(failed_restore);
    ASSERT_EQ(failed_restore->status, 202);
    const std::string failed_id = response_body(failed_restore)["task_id"];
    EXPECT_EQ(wait_for_terminal(client, failed_id)["status"], "FAILED");
    const auto failed_events = client.Get("/api/tasks/" + failed_id + "/events");
    ASSERT_TRUE(failed_events);
    EXPECT_NE(failed_events->body.find("event: error"), std::string::npos);
}

TEST(ProjectAcceptance, StreamsTaskEventsAndSupportsCursorOverHttp) {
    HttpHarness harness;
    ASSERT_TRUE(harness.start());
    auto client = harness.client();
    const std::string source = harness.temp.create_dir("source");
    const std::string output = harness.temp.create_dir("output");
    harness.temp.create_file("source/file.txt", "event data");
    const auto response = client.Post(
        "/api/backup", backup_request(source, output).dump(), "application/json");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 202);
    const std::string task_id = response_body(response)["task_id"];
    ASSERT_EQ(wait_for_terminal(client, task_id)["status"], "SUCCESS");

    const auto events = client.Get("/api/tasks/" + task_id + "/events");
    ASSERT_TRUE(events);
    EXPECT_EQ(events->status, 200);
    EXPECT_EQ(events->get_header_value("Content-Type"),
              "text/event-stream; charset=utf-8");
    EXPECT_NE(events->body.find("event: status"), std::string::npos);
    EXPECT_NE(events->body.find("event: result"), std::string::npos);
    const auto first_id = events->body.find("id: ");
    ASSERT_NE(first_id, std::string::npos);
    const auto first_end = events->body.find('\n', first_id);
    const std::string cursor = events->body.substr(first_id + 4, first_end - first_id - 4);
    const auto resumed = client.Get("/api/tasks/" + task_id + "/events",
                                    httplib::Headers{{"Last-Event-ID", cursor}});
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->status, 200);
}

TEST(ProjectAcceptance, RestoresDeviceEntriesWhenMknodIsAvailable) {
    TempDir temp;
    const auto source = std::filesystem::path(temp.create_dir("source"));
    const auto output = std::filesystem::path(temp.create_dir("output"));
    const auto target = std::filesystem::path(temp.create_dir("target"));
    const auto character = source / "character-device";
    const auto block = source / "block-device";
    if (!create_device(character, false, 1, 7) || !create_device(block, true, 7, 0)) {
        GTEST_SKIP() << "device creation requires root or CAP_MKNOD: " << std::strerror(errno);
    }

    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    ApiConfig config;
    config.port = 0;
    config.allowed_roots = {temp.path()};
    WebApiServer server(runtime, config);
    ASSERT_TRUE(server.start());
    httplib::Client client("127.0.0.1", server.port());
    const auto backup = client.Post(
        "/api/backup", backup_request(source.string(), output.string(), "devices.dat").dump(),
        "application/json");
    ASSERT_TRUE(backup);
    ASSERT_EQ(backup->status, 202);
    ASSERT_EQ(wait_for_terminal(client, response_body(backup)["task_id"])["status"], "SUCCESS");
    const auto restore = client.Post(
        "/api/restore", restore_request((output / "devices.dat").string(),
                                         target.string(), "OVERWRITE").dump(),
        "application/json");
    ASSERT_TRUE(restore);
    ASSERT_EQ(restore->status, 202);
    ASSERT_EQ(wait_for_terminal(client, response_body(restore)["task_id"])["status"], "SUCCESS");

    struct stat character_stat{};
    struct stat block_stat{};
    ASSERT_EQ(::lstat((target / "character-device").c_str(), &character_stat), 0);
    ASSERT_EQ(::lstat((target / "block-device").c_str(), &block_stat), 0);
    EXPECT_TRUE(S_ISCHR(character_stat.st_mode));
    EXPECT_TRUE(S_ISBLK(block_stat.st_mode));
    EXPECT_EQ(major(character_stat.st_rdev), 1u);
    EXPECT_EQ(minor(character_stat.st_rdev), 7u);
    EXPECT_EQ(major(block_stat.st_rdev), 7u);
    EXPECT_EQ(minor(block_stat.st_rdev), 0u);
}
