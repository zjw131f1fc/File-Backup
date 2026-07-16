#include <gtest/gtest.h>
#include "scheduler/task_runtime.h"
#include "../../tests/helpers/temp_dir.h"
#include <chrono>
#include <filesystem>
#include <thread>

using namespace backup;
using namespace backup::testing;

namespace {

Task wait_for_terminal(TaskManager& task_manager, const std::string& task_id) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        Task task = task_manager.get_task(task_id);
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

TEST(TaskRuntime, SubmitsBackupAndRunsInBackground) {
    TempDir tmp;
    const std::string source = tmp.create_dir("source");
    const std::string archive = tmp.path() + "/backup.dat";

    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    runtime.start();

    BackupRequest request;
    request.source_path = source;
    request.output_path = archive;
    TaskSubmission submission = runtime.submit_backup(request);

    ASSERT_TRUE(submission.accepted());
    Task task = wait_for_terminal(task_manager, submission.task_id);
    EXPECT_EQ(task.status, TaskStatus::SUCCESS);
    EXPECT_TRUE(std::filesystem::exists(archive));

    runtime.shutdown();
}

TEST(TaskRuntime, RunsMultipleTasks) {
    TempDir tmp;
    const std::string source = tmp.create_dir("source");
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 2, 4);
    runtime.start();

    BackupRequest first;
    first.source_path = source;
    first.output_path = tmp.path() + "/first.dat";
    BackupRequest second = first;
    second.output_path = tmp.path() + "/second.dat";

    TaskSubmission first_submission = runtime.submit_backup(first);
    TaskSubmission second_submission = runtime.submit_backup(second);
    ASSERT_TRUE(first_submission.accepted());
    ASSERT_TRUE(second_submission.accepted());

    EXPECT_EQ(wait_for_terminal(task_manager, first_submission.task_id).status,
              TaskStatus::SUCCESS);
    EXPECT_EQ(wait_for_terminal(task_manager, second_submission.task_id).status,
              TaskStatus::SUCCESS);
    EXPECT_EQ(runtime.queued_task_count(), 0u);

    runtime.shutdown();
}

TEST(TaskRuntime, RejectsQueuedTasksAfterQueueLimit) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 1);

    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";
    ASSERT_TRUE(runtime.submit_backup(request).accepted());

    TaskSubmission rejected = runtime.submit_backup(request);
    EXPECT_FALSE(rejected.accepted());
    EXPECT_EQ(rejected.result.status, Status::FAILED);
    EXPECT_NE(rejected.result.message.find("queue"), std::string::npos);
}

TEST(TaskRuntime, RejectsSubmissionAfterShutdown) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 1);
    runtime.shutdown();

    BackupRequest request;
    TaskSubmission submission = runtime.submit_backup(request);
    EXPECT_FALSE(submission.accepted());
    EXPECT_EQ(submission.result.status, Status::FAILED);
}

TEST(TaskRuntime, ListsTaskMetadata) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);

    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";
    ASSERT_TRUE(runtime.submit_backup(request).accepted());

    const auto tasks = runtime.list_tasks();
    ASSERT_EQ(tasks.size(), 1u);
    EXPECT_EQ(tasks.front().type, "backup");
    EXPECT_EQ(tasks.front().task.status, TaskStatus::PENDING);
    EXPECT_FALSE(tasks.front().created_at.empty());
}

TEST(TaskRuntime, RejectsDuplicateBackupOutput) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);

    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";
    ASSERT_TRUE(runtime.submit_backup(request).accepted());

    const TaskSubmission rejected = runtime.submit_backup(request);
    EXPECT_FALSE(rejected.accepted());
    EXPECT_EQ(rejected.error_code, "OUTPUT_CONFLICT");
}

TEST(TaskRuntime, CapturesTaskEvents) {
    TempDir tmp;
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    runtime.start();

    BackupRequest request;
    request.source_path = tmp.create_dir("source");
    request.output_path = tmp.path() + "/archive.dat";
    const TaskSubmission submission = runtime.submit_backup(request);
    ASSERT_TRUE(submission.accepted());
    wait_for_terminal(task_manager, submission.task_id);

    const auto events = runtime.get_events(submission.task_id);
    ASSERT_FALSE(events.empty());
    for (std::size_t index = 1; index < events.size(); ++index) {
        EXPECT_LT(events[index - 1].id, events[index].id);
    }
    EXPECT_EQ(events.back().task.status, TaskStatus::SUCCESS);
    runtime.shutdown();
}
