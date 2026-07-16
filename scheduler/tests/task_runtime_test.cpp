#include <gtest/gtest.h>
#include "scheduler/task_runtime.h"
#include "modules/archive_reader/archive_reader.h"
#include "modules/archive_writer/archive_writer.h"
#include "modules/filter/filter.h"
#include "modules/restore/restore.h"
#include "modules/scanner/scanner.h"
#include "../../tests/helpers/temp_dir.h"
#include <chrono>
#include <filesystem>
#include <stdexcept>
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

TEST(TaskRuntime, RejectsInvalidConfiguration) {
    TaskManager task_manager;
    EXPECT_THROW(TaskRuntime(task_manager, 0, 1), std::invalid_argument);
    EXPECT_THROW(TaskRuntime(task_manager, 1, 0), std::invalid_argument);
}

TEST(TaskRuntime, RepeatedStartAndShutdownAreSafe) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 2);

    runtime.start();
    runtime.start();
    runtime.shutdown();
    runtime.start();
    runtime.shutdown();
}

TEST(TaskRuntime, ShutdownCancelsQueuedTasks) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 2);
    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";
    const auto submission = runtime.submit_backup(request);

    ASSERT_TRUE(submission.accepted());
    runtime.shutdown();

    EXPECT_EQ(task_manager.get_task(submission.task_id).status,
              TaskStatus::CANCELLED);
}

TEST(TaskRuntime, CompletesBackupWhenFactoryReturnsNull) {
    TaskManager task_manager;
    TaskRuntimeFactories factories;
    factories.scanner = [] { return std::unique_ptr<IScanner>(); };
    TaskRuntime runtime(task_manager, 1, 2, std::move(factories));
    runtime.start();
    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";

    const auto submission = runtime.submit_backup(request);
    ASSERT_TRUE(submission.accepted());
    const auto task = wait_for_terminal(task_manager, submission.task_id);

    EXPECT_EQ(task.status, TaskStatus::FAILED);
    EXPECT_NE(task.result.message.find("create backup task modules"), std::string::npos);
    runtime.shutdown();
}

TEST(TaskRuntime, CompletesRestoreWhenFactoryReturnsNull) {
    TaskManager task_manager;
    TaskRuntimeFactories factories;
    factories.archive_reader = [](const std::string&) {
        return std::unique_ptr<IArchiveReader>();
    };
    TaskRuntime runtime(task_manager, 1, 2, std::move(factories));
    runtime.start();
    RestoreRequest request;
    request.archive_path = "/archive";
    request.target_path = "/target";

    const auto submission = runtime.submit_restore(request);
    ASSERT_TRUE(submission.accepted());
    const auto task = wait_for_terminal(task_manager, submission.task_id);

    EXPECT_EQ(task.status, TaskStatus::FAILED);
    EXPECT_NE(task.result.message.find("create restore task modules"), std::string::npos);
    runtime.shutdown();
}

TEST(TaskRuntime, ConvertsFactoryExceptionToTaskFailure) {
    TaskManager task_manager;
    TaskRuntimeFactories factories;
    factories.scanner = []() -> std::unique_ptr<IScanner> {
        throw std::runtime_error("factory failed");
    };
    TaskRuntime runtime(task_manager, 1, 2, std::move(factories));
    runtime.start();
    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";

    const auto submission = runtime.submit_backup(request);
    ASSERT_TRUE(submission.accepted());
    const auto task = wait_for_terminal(task_manager, submission.task_id);

    EXPECT_EQ(task.status, TaskStatus::FAILED);
    EXPECT_NE(task.result.message.find("factory failed"), std::string::npos);
    runtime.shutdown();
}

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

TEST(TaskRuntime, RunsRestoreAndRecordsFailure) {
    TempDir tmp;
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    runtime.start();

    RestoreRequest request;
    request.archive_path = tmp.path() + "/missing.archive";
    request.target_path = tmp.create_dir("restore");
    request.conflict_policy = ConflictPolicy::SKIP;
    const TaskSubmission submission = runtime.submit_restore(request);

    ASSERT_TRUE(submission.accepted());
    const Task task = wait_for_terminal(task_manager, submission.task_id);
    EXPECT_EQ(task.status, TaskStatus::FAILED);
    EXPECT_EQ(runtime.list_tasks().front().type, "restore");
    EXPECT_FALSE(runtime.list_tasks().front().finished_at.empty());
    runtime.shutdown();
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

TEST(TaskRuntime, CancelsPendingTaskAndFiltersEvents) {
    TaskManager task_manager;
    TaskRuntime runtime(task_manager, 1, 4);
    BackupRequest request;
    request.source_path = "/source";
    request.output_path = "/archive";
    const TaskSubmission submission = runtime.submit_backup(request);

    ASSERT_TRUE(submission.accepted());
    ASSERT_TRUE(runtime.cancel_task(submission.task_id));
    EXPECT_FALSE(runtime.cancel_task(submission.task_id));

    const auto events = runtime.get_events(submission.task_id);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().task.status, TaskStatus::CANCELLED);
    EXPECT_TRUE(runtime.get_events(submission.task_id, events.back().id).empty());
    EXPECT_TRUE(runtime.get_events("missing-task").empty());
}
