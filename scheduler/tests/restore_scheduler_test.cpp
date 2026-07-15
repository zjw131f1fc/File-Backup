#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "scheduler/restore_scheduler.h"
#include "scheduler/task_manager.h"
#include "../../tests/mocks/mock_modules.h"

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

    // 第一个条目正常，第二个条目前检查到取消
    {
        InSequence seq;
        EXPECT_CALL(mock_reader, validate())
            .WillOnce(Return(Result{Status::SUCCESS, ""}));
        EXPECT_CALL(mock_reader, has_next_entry())
            .WillOnce(Return(true));    // 第一个条目存在
        EXPECT_CALL(mock_reader, next_entry(_))
            .WillOnce(Return(Result{Status::SUCCESS, ""}));
        EXPECT_CALL(mock_restorer, restore_entry(_, _, _, _))
            .WillOnce(Return(Result{Status::SUCCESS, ""}));
        EXPECT_CALL(mock_restorer, restore_metadata(_, _))
            .WillOnce(Return(Result{Status::SUCCESS, ""}));
        // 取消后不再调用
    }

    RestoreScheduler scheduler(task_mgr, &mock_reader, &mock_restorer);
    RestoreRequest req;
    req.archive_path = "/tmp/archive.dat";
    req.target_path = "/tmp/restore";

    std::string task_id = task_mgr.create_restore_task(req);
    // 在执行前取消
    task_mgr.cancel_task(task_id);

    Result r = scheduler.run(task_id, req);
    EXPECT_EQ(r.status, Status::CANCELLED);

    Task task = task_mgr.get_task(task_id);
    EXPECT_EQ(task.status, TaskStatus::CANCELLED);
}
