#include <gtest/gtest.h>
#include "scheduler/task_manager.h"
#include "common/backup_request.h"
#include "common/restore_request.h"

using namespace backup;

TEST(TaskManager, CreateBackupTask) {
    TaskManager mgr;
    BackupRequest req;
    req.source_path = "/tmp/src";
    req.output_path = "/tmp/archive.dat";
    std::string id = mgr.create_backup_task(req);
    EXPECT_FALSE(id.empty());

    Task task = mgr.get_task(id);
    EXPECT_EQ(task.task_id, id);
    EXPECT_EQ(task.status, TaskStatus::PENDING);
}

TEST(TaskManager, CreateRestoreTask) {
    TaskManager mgr;
    RestoreRequest req;
    req.archive_path = "/tmp/archive.dat";
    req.target_path = "/tmp/restore";
    std::string id = mgr.create_restore_task(req);
    EXPECT_FALSE(id.empty());

    Task task = mgr.get_task(id);
    EXPECT_EQ(task.task_id, id);
    EXPECT_EQ(task.status, TaskStatus::PENDING);
}

TEST(TaskManager, GetNonExistentTask) {
    TaskManager mgr;
    Task task = mgr.get_task("nonexistent");
    EXPECT_EQ(task.status, TaskStatus::FAILED);
    EXPECT_NE(task.result.message.find("task not found"), std::string::npos);
}

TEST(TaskManager, CancelPendingTask) {
    TaskManager mgr;
    BackupRequest req;
    std::string id = mgr.create_backup_task(req);
    EXPECT_TRUE(mgr.cancel_task(id));

    Task task = mgr.get_task(id);
    EXPECT_EQ(task.status, TaskStatus::CANCELLED);
}

TEST(TaskManager, CancelCompletedTaskFails) {
    TaskManager mgr;
    BackupRequest req;
    std::string id = mgr.create_backup_task(req);

    Result r;
    r.status = Status::SUCCESS;
    mgr.complete_task(id, r);

    EXPECT_FALSE(mgr.cancel_task(id));
    Task task = mgr.get_task(id);
    EXPECT_EQ(task.status, TaskStatus::SUCCESS);
}

TEST(TaskManager, CompleteTaskSuccess) {
    TaskManager mgr;
    BackupRequest req;
    std::string id = mgr.create_backup_task(req);

    Result r;
    r.status = Status::SUCCESS;
    r.message = "done";
    mgr.complete_task(id, r);

    Task task = mgr.get_task(id);
    EXPECT_EQ(task.status, TaskStatus::SUCCESS);
    EXPECT_EQ(task.result.status, Status::SUCCESS);
}

TEST(TaskManager, CompleteTaskPartialSuccess) {
    TaskManager mgr;
    BackupRequest req;
    std::string id = mgr.create_backup_task(req);

    Result r;
    r.status = Status::PARTIAL_SUCCESS;
    r.error_count = 2;
    mgr.complete_task(id, r);

    Task task = mgr.get_task(id);
    EXPECT_EQ(task.status, TaskStatus::PARTIAL_SUCCESS);
}

TEST(TaskManager, UpdateProgress) {
    TaskManager mgr;
    BackupRequest req;
    std::string id = mgr.create_backup_task(req);

    Progress p;
    p.stage = "scanning";
    p.processed_entries = 42;
    p.current_path = "/tmp/src/dir";
    mgr.update_progress(id, p);

    Task task = mgr.get_task(id);
    EXPECT_EQ(task.progress.stage, "scanning");
    EXPECT_EQ(task.progress.processed_entries, 42u);
    EXPECT_EQ(task.progress.current_path, "/tmp/src/dir");
}

TEST(TaskManager, GenerateTaskIdUnique) {
    std::string id1 = TaskManager::generate_task_id();
    std::string id2 = TaskManager::generate_task_id();
    EXPECT_NE(id1, id2);
}

// ===== 补充：多任务独立 =====
TEST(TaskManager, MultipleTasksIndependent) {
    TaskManager mgr;
    BackupRequest req1;
    req1.source_path = "/src1";
    BackupRequest req2;
    req2.source_path = "/src2";

    std::string id1 = mgr.create_backup_task(req1);
    std::string id2 = mgr.create_backup_task(req2);

    // 完成 task1，task2 仍是 PENDING
    Result r1;
    r1.status = Status::SUCCESS;
    mgr.complete_task(id1, r1);

    EXPECT_EQ(mgr.get_task(id1).status, TaskStatus::SUCCESS);
    EXPECT_EQ(mgr.get_task(id2).status, TaskStatus::PENDING);
}

// ===== 补充：进度更新可见 =====
TEST(TaskManager, ProgressUpdatesAreVisible) {
    TaskManager mgr;
    BackupRequest req;
    std::string id = mgr.create_backup_task(req);

    Progress p1;
    p1.stage = "scanning";
    p1.processed_entries = 10;
    p1.processed_bytes = 500;
    p1.current_path = "/src/dir";
    mgr.update_progress(id, p1);

    Progress p2;
    p2.stage = "writing";
    p2.processed_entries = 20;
    p2.processed_bytes = 1000;
    p2.current_path = "/src/file.txt";
    mgr.update_progress(id, p2);

    Task task = mgr.get_task(id);
    EXPECT_EQ(task.progress.stage, "writing");
    EXPECT_EQ(task.progress.processed_entries, 20u);
    EXPECT_EQ(task.progress.processed_bytes, 1000u);
    EXPECT_EQ(task.progress.current_path, "/src/file.txt");
}
