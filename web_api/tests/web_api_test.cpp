#include <gtest/gtest.h>
#include "web_api/web_api.h"
#include "../../tests/helpers/temp_dir.h"
#include <nlohmann/json.hpp>

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
