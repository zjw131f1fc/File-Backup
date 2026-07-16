#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "scheduler/restore_scheduler.h"
#include "scheduler/task_manager.h"
#include "modules/archive_writer/archive_writer.h"
#include "../../tests/mocks/mock_modules.h"
#include "../../tests/helpers/temp_dir.h"

using namespace backup;
using namespace backup::testing;
using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;
using ::testing::InSequence;

// ===== 4. 还原取消在循环中退出 =====
TEST(RestoreSchedulerContract, CancelStopsLoop) {
    TaskManager task_mgr;
    NiceMock<MockArchiveReader> mock_reader;
    NiceMock<MockRestorer> mock_restorer;

    RestoreRequest req;
    req.archive_path = "/tmp/archive.dat";
    req.target_path = "/tmp/restore";

    std::string task_id = task_mgr.create_restore_task(req);

    // 第一个条目正常，处理完成后在第二个条目前取消
    {
        InSequence seq;
        EXPECT_CALL(mock_reader, validate())
            .WillOnce(Return(Result{Status::SUCCESS, ""}));
        EXPECT_CALL(mock_reader, has_next_entry())
            .WillOnce(Return(true));    // 第一个条目存在
        EXPECT_CALL(mock_reader, next_entry(_))
            .WillOnce(::testing::Invoke([&task_mgr, &task_id](EntryInfo& entry) {
                entry.path = "file.txt";
                task_mgr.cancel_task(task_id);
                return Result{Status::SUCCESS, ""};
            }));
        EXPECT_CALL(mock_restorer, restore_entry(_, _, _, _))
            .WillOnce(Return(Result{Status::SUCCESS, ""}));
        EXPECT_CALL(mock_restorer, restore_metadata(_, _))
            .WillOnce(Return(Result{Status::SUCCESS, ""}));
        EXPECT_CALL(mock_reader, has_next_entry())
            .WillOnce(Return(true));    // 第二个条目前仍有条目
        // 取消后不再调用
    }

    RestoreScheduler scheduler(task_mgr, mock_reader, mock_restorer);
    Result r = scheduler.run(task_id, req);
    EXPECT_EQ(r.status, Status::CANCELLED);

    Task task = task_mgr.get_task(task_id);
    EXPECT_EQ(task.status, TaskStatus::CANCELLED);
}

TEST(RestoreSchedulerContract, ValidationFailureCompletesTaskAsFailed) {
    TaskManager task_mgr;
    NiceMock<MockArchiveReader> mock_reader;
    NiceMock<MockRestorer> mock_restorer;
    Result validation;
    validation.status = Status::FAILED;
    validation.message = "bad archive";
    EXPECT_CALL(mock_reader, validate()).WillOnce(Return(validation));

    RestoreRequest req;
    req.archive_path = "/tmp/archive.dat";
    req.target_path = "/tmp/restore";
    const auto task_id = task_mgr.create_restore_task(req);

    RestoreScheduler scheduler(task_mgr, mock_reader, mock_restorer);
    const Result result = scheduler.run(task_id, req);
    EXPECT_EQ(result.status, Status::FAILED);
    EXPECT_NE(result.message.find("bad archive"), std::string::npos);
    EXPECT_EQ(task_mgr.get_task(task_id).status, TaskStatus::FAILED);
}

TEST(RestoreSchedulerContract, EntryFailureProducesPartialSuccess) {
    TaskManager task_mgr;
    NiceMock<MockArchiveReader> mock_reader;
    NiceMock<MockRestorer> mock_restorer;
    Result validation;
    validation.status = Status::SUCCESS;
    EXPECT_CALL(mock_reader, validate()).WillOnce(Return(validation));
    EXPECT_CALL(mock_reader, has_next_entry())
        .WillOnce(Return(true)).WillOnce(Return(false));
    EXPECT_CALL(mock_reader, next_entry(_))
        .WillOnce(::testing::Invoke([](EntryInfo& entry) {
            entry.path = "broken.txt";
            entry.type = EntryType::REGULAR_FILE;
            Result result;
            result.status = Status::SUCCESS;
            return result;
        }));
    Result failure;
    failure.status = Status::FAILED;
    failure.message = "cannot write";
    EXPECT_CALL(mock_restorer, restore_entry(_, _, _, _))
        .WillOnce(Return(failure));

    RestoreRequest req;
    req.archive_path = "/tmp/archive.dat";
    req.target_path = "/tmp/restore";
    const auto task_id = task_mgr.create_restore_task(req);

    RestoreScheduler scheduler(task_mgr, mock_reader, mock_restorer);
    const Result result = scheduler.run(task_id, req);
    EXPECT_EQ(result.status, Status::PARTIAL_SUCCESS);
    EXPECT_EQ(result.error_count, 1);
}

TEST(RestoreSchedulerContract, MetadataWarningIsPreserved) {
    TaskManager task_mgr;
    NiceMock<MockArchiveReader> mock_reader;
    NiceMock<MockRestorer> mock_restorer;
    Result success;
    success.status = Status::SUCCESS;
    EXPECT_CALL(mock_reader, validate()).WillOnce(Return(success));
    EXPECT_CALL(mock_reader, has_next_entry())
        .WillOnce(Return(true)).WillOnce(Return(false));
    EXPECT_CALL(mock_reader, next_entry(_))
        .WillOnce(::testing::Invoke([](EntryInfo& entry) {
            entry.path = "file.txt";
            entry.type = EntryType::REGULAR_FILE;
            Result result;
            result.status = Status::SUCCESS;
            return result;
        }));
    EXPECT_CALL(mock_restorer, restore_entry(_, _, _, _))
        .WillOnce(Return(success));
    Result warning;
    warning.status = Status::FAILED;
    warning.warning_count = 1;
    warning.message = "owner unavailable";
    EXPECT_CALL(mock_restorer, restore_metadata(_, _))
        .WillOnce(Return(warning));

    RestoreRequest req;
    req.archive_path = "/tmp/archive.dat";
    req.target_path = "/tmp/restore";
    const auto task_id = task_mgr.create_restore_task(req);

    RestoreScheduler scheduler(task_mgr, mock_reader, mock_restorer);
    const Result result = scheduler.run(task_id, req);
    EXPECT_EQ(result.status, Status::SUCCESS);
    EXPECT_EQ(result.warning_count, 1);
}

TEST(RestoreSchedulerStubIntegration, DefaultStubsRestoreEmptyArchive) {
    TempDir archive_tmp;
    TempDir restore_tmp;
    const auto archive_path = archive_tmp.path() + "/archive.dat";

    auto writer = create_archive(archive_path);
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(writer->commit().status, Status::SUCCESS);

    TaskManager task_mgr;
    RestoreRequest req;
    req.archive_path = archive_path;
    req.target_path = restore_tmp.path();

    auto reader = open_archive(archive_path);
    auto restorer = create_restorer();
    RestoreScheduler scheduler(task_mgr, *reader, *restorer);

    const auto task_id = task_mgr.create_restore_task(req);
    Result result = scheduler.run(task_id, req);

    EXPECT_EQ(result.status, Status::SUCCESS);
    EXPECT_EQ(task_mgr.get_task(task_id).status, TaskStatus::SUCCESS);
}
