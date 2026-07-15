#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "scheduler/backup_scheduler.h"
#include "scheduler/restore_scheduler.h"
#include "scheduler/task_manager.h"
#include "../../tests/mocks/mock_modules.h"
#include "../../tests/helpers/temp_dir.h"

using namespace backup;
using namespace backup::testing;
using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;
using ::testing::StrictMock;
using ::testing::InSequence;

// ===== 1. Scanner 失败时调 abort =====
TEST(BackupSchedulerContract, FailCallsAbort) {
    TaskManager task_mgr;
    StrictMock<MockScanner> mock_scanner;
    StrictMock<MockArchiveWriter> mock_writer;

    // Scanner 返回 FAILED
    Result fail_result;
    fail_result.status = Status::FAILED;
    fail_result.message = "scan failed";
    EXPECT_CALL(mock_scanner, scan_and_backup(_, _, _, _))
        .WillOnce(Return(fail_result));

    // abort 应被调用
    Result abort_result;
    abort_result.status = Status::SUCCESS;
    EXPECT_CALL(mock_writer, abort())
        .WillOnce(Return(abort_result));

    // commit 不应被调用（StrictMock 会报错如果被调）

    BackupScheduler scheduler(task_mgr, &mock_scanner, &mock_writer);
    BackupRequest req;
    req.source_path = "/tmp/src";
    req.output_path = "/tmp/archive.dat";

    std::string task_id = task_mgr.create_backup_task(req);
    Result r = scheduler.run(task_id, req);
    EXPECT_EQ(r.status, Status::FAILED);

    // 任务最终状态应为 FAILED
    Task task = task_mgr.get_task(task_id);
    EXPECT_EQ(task.status, TaskStatus::FAILED);
}

// ===== 2. Scanner 成功时调 commit =====
TEST(BackupSchedulerContract, SuccessCallsCommit) {
    TaskManager task_mgr;
    NiceMock<MockScanner> mock_scanner;
    NiceMock<MockArchiveWriter> mock_writer;

    Result success_result;
    success_result.status = Status::SUCCESS;
    EXPECT_CALL(mock_scanner, scan_and_backup(_, _, _, _))
        .WillOnce(Return(success_result));

    Result commit_result;
    commit_result.status = Status::SUCCESS;
    EXPECT_CALL(mock_writer, commit())
        .WillOnce(Return(commit_result));

    // abort 不应被调用
    EXPECT_CALL(mock_writer, abort()).Times(0);

    BackupScheduler scheduler(task_mgr, &mock_scanner, &mock_writer);
    BackupRequest req;
    req.source_path = "/tmp/src";
    req.output_path = "/tmp/archive.dat";

    std::string task_id = task_mgr.create_backup_task(req);
    Result r = scheduler.run(task_id, req);
    EXPECT_EQ(r.status, Status::SUCCESS);

    Task task = task_mgr.get_task(task_id);
    EXPECT_EQ(task.status, TaskStatus::SUCCESS);
}

// ===== 3. 取消停止扫描 =====
TEST(BackupSchedulerContract, CancelStopsScan) {
    TaskManager task_mgr;
    NiceMock<MockScanner> mock_scanner;
    NiceMock<MockArchiveWriter> mock_writer;

    // Scanner 收到取消信号（进度回调返回 false）后返回 CANCELLED
    Result cancelled_result;
    cancelled_result.status = Status::CANCELLED;
    EXPECT_CALL(mock_scanner, scan_and_backup(_, _, _, _))
        .WillOnce(Return(cancelled_result));

    EXPECT_CALL(mock_writer, abort())
        .WillOnce(Return(Result{}));

    BackupScheduler scheduler(task_mgr, &mock_scanner, &mock_writer);
    BackupRequest req;
    req.source_path = "/tmp/src";
    req.output_path = "/tmp/archive.dat";

    std::string task_id = task_mgr.create_backup_task(req);
    // 取消任务
    task_mgr.cancel_task(task_id);

    Result r = scheduler.run(task_id, req);
    EXPECT_EQ(r.status, Status::CANCELLED);

    Task task = task_mgr.get_task(task_id);
    EXPECT_EQ(task.status, TaskStatus::CANCELLED);
}

TEST(BackupSchedulerStubIntegration, DefaultStubsCreateArchive) {
    TempDir tmp;
    const auto source_path = tmp.create_dir("source");
    const auto archive_path = tmp.path() + "/archive.dat";

    TaskManager task_mgr;
    BackupScheduler scheduler(task_mgr);
    BackupRequest req;
    req.source_path = source_path;
    req.output_path = archive_path;

    const auto task_id = task_mgr.create_backup_task(req);
    Result result = scheduler.run(task_id, req);

    EXPECT_EQ(result.status, Status::SUCCESS);
    EXPECT_TRUE(std::filesystem::exists(archive_path));
    EXPECT_EQ(task_mgr.get_task(task_id).status, TaskStatus::SUCCESS);
}
