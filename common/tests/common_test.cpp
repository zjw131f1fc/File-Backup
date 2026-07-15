#include <gtest/gtest.h>
#include "common/status.h"
#include "common/result.h"
#include "common/progress.h"
#include "common/conflict_policy.h"
#include "common/entry_type.h"
#include "common/entry_info.h"
#include "common/filter_rules.h"
#include "common/backup_request.h"
#include "common/restore_request.h"

using namespace backup;

TEST(Common, StatusValues) {
    EXPECT_EQ(static_cast<int>(Status::SUCCESS),      0);
    EXPECT_EQ(static_cast<int>(Status::FAILED),       1);
    EXPECT_EQ(static_cast<int>(Status::PARTIAL_SUCCESS), 2);
    EXPECT_EQ(static_cast<int>(Status::CANCELLED),    3);
}

TEST(Common, ResultDefaultIsSuccess) {
    Result r;
    EXPECT_EQ(r.status, Status::SUCCESS);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.error_count, 0);
    EXPECT_EQ(r.warning_count, 0);
    EXPECT_TRUE(r.message.empty());
}

TEST(Common, ResultFailedNotOk) {
    Result r;
    r.status = Status::FAILED;
    r.error_count = 2;
    EXPECT_FALSE(r.ok());
}

TEST(Common, ProgressDefaults) {
    Progress p;
    EXPECT_EQ(p.processed_entries, 0u);
    EXPECT_EQ(p.processed_bytes, 0u);
    EXPECT_TRUE(p.stage.empty());
    EXPECT_TRUE(p.current_path.empty());
}

TEST(Common, ConflictPolicyValues) {
    ConflictPolicy skip    = ConflictPolicy::SKIP;
    ConflictPolicy overwrite = ConflictPolicy::OVERWRITE;
    ConflictPolicy rename  = ConflictPolicy::RENAME;
    EXPECT_NE(skip, overwrite);
    EXPECT_NE(overwrite, rename);
}

TEST(Common, EntryTypeCoverage) {
    // 确保所有类型都有定义
    EntryType types[] = {
        EntryType::REGULAR_FILE,
        EntryType::DIRECTORY,
        EntryType::SYMBOLIC_LINK,
        EntryType::HARD_LINK,
        EntryType::FIFO,
        EntryType::CHARACTER_DEVICE,
        EntryType::BLOCK_DEVICE,
    };
    EXPECT_EQ(sizeof(types) / sizeof(types[0]), 7u);
}

TEST(Common, EntryInfoDefaults) {
    EntryInfo e;
    EXPECT_EQ(e.type, EntryType::REGULAR_FILE);
    EXPECT_EQ(e.size, 0u);
    EXPECT_EQ(e.permissions, static_cast<mode_t>(0));
    EXPECT_TRUE(e.path.empty());
    EXPECT_TRUE(e.link_target.empty());
    EXPECT_TRUE(e.hard_link_target.empty());
}

TEST(Common, FilterRulesDefaults) {
    FilterRules f;
    EXPECT_TRUE(f.include_paths.empty());
    EXPECT_TRUE(f.exclude_paths.empty());
    EXPECT_TRUE(f.include_types.empty());
    EXPECT_TRUE(f.include_names.empty());
    EXPECT_TRUE(f.exclude_names.empty());
    EXPECT_EQ(f.newer_than_sec, 0);
    EXPECT_EQ(f.older_than_sec, 0);
    EXPECT_EQ(f.min_size, 0u);
    EXPECT_EQ(f.max_size, 0u);
    EXPECT_TRUE(f.include_uids.empty());
}

TEST(Common, BackupRequestFields) {
    BackupRequest req;
    req.source_path = "/home/user/data";
    req.output_path = "/tmp/backup.dat";
    EXPECT_EQ(req.source_path, "/home/user/data");
    EXPECT_EQ(req.output_path, "/tmp/backup.dat");
}

TEST(Common, RestoreRequestDefaults) {
    RestoreRequest req;
    EXPECT_EQ(req.conflict_policy, ConflictPolicy::SKIP);
    EXPECT_TRUE(req.archive_path.empty());
    EXPECT_TRUE(req.target_path.empty());
}
