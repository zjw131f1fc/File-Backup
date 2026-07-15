#include <gtest/gtest.h>
#include "modules/filter/filter.h"

using namespace backup;

// ============================================================
// 筛选子模块 — 功能测试
// 覆盖契约测试之外的边界条件、组合场景、真实备份场景
// ============================================================

// ===== 1. Glob 匹配边界测试 =====
TEST(FilterFunctional, GlobExactMatch) {
    FilterRules rules;
    rules.include_names = {"exact.txt"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "exact.txt";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "exact.txt.bak";
    EXPECT_FALSE(filter->should_include(e));
}

TEST(FilterFunctional, GlobQuestionMark) {
    FilterRules rules;
    rules.include_names = {"file.???"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "file.txt";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "file.ab";
    EXPECT_FALSE(filter->should_include(e));  // 只有2字符

    e.path = "file.abcd";
    EXPECT_FALSE(filter->should_include(e));  // 有4字符
}

TEST(FilterFunctional, GlobStarAtStart) {
    FilterRules rules;
    rules.include_names = {"*.bak"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "backup.bak";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "backup.bakup";
    EXPECT_FALSE(filter->should_include(e));
}

TEST(FilterFunctional, GlobStarAtEnd) {
    FilterRules rules;
    rules.include_names = {"tmp_*"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "tmp_123";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "temp_123";
    EXPECT_FALSE(filter->should_include(e));
}

TEST(FilterFunctional, GlobMultipleStars) {
    FilterRules rules;
    rules.include_names = {"a*b*c"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "abc";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "aXbYc";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "aXbYcZ";
    EXPECT_FALSE(filter->should_include(e));
}

TEST(FilterFunctional, GlobStarOnly) {
    FilterRules rules;
    rules.include_names = {"*"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "anything.xyz";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "a";
    EXPECT_TRUE(filter->should_include(e));
}

TEST(FilterFunctional, GlobMixedWildcards) {
    FilterRules rules;
    rules.include_names = {"test_?.log"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "test_1.log";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "test_ab.log";
    EXPECT_FALSE(filter->should_include(e));  // ? 只匹配一个字符

    e.path = "test_.log";
    EXPECT_FALSE(filter->should_include(e));  // ? 必须匹配一个字符
}

// ===== 2. 路径筛选边界测试 =====
TEST(FilterFunctional, PathIncludeRootLevel) {
    FilterRules rules;
    rules.include_paths = {"README.md"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "README.md";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "README.md.bak";
    EXPECT_FALSE(filter->should_include(e));  // 不是路径前缀匹配
}

TEST(FilterFunctional, PathExcludeDeepNested) {
    FilterRules rules;
    rules.include_paths = {"project/"};
    rules.exclude_paths = {"project/node_modules/"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "project/src/main.cpp";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "project/node_modules/express/index.js";
    EXPECT_FALSE(filter->should_include(e));

    e.path = "project/node_modules/test.js";
    EXPECT_FALSE(filter->should_include(e));
}

TEST(FilterFunctional, PathExcludePrefixNotPartial) {
    FilterRules rules;
    rules.exclude_paths = {"build"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "build/output.o";
    EXPECT_FALSE(filter->should_include(e));

    // "build" 不匹配 "build_v2"（因为前缀后跟 _ 不是 /）
    e.path = "build_v2/output.o";
    EXPECT_TRUE(filter->should_include(e));
}

TEST(FilterFunctional, PathWithTrailingSlash) {
    FilterRules rules;
    rules.exclude_paths = {"temp/"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "temp/file.tmp";
    EXPECT_FALSE(filter->should_include(e));

    // "temp" 目录本身
    e.path = "temp";
    EXPECT_TRUE(filter->should_include(e));  // path == "temp", prefix = "temp/" 不匹配因为prefix末尾是/但path没有/
}

// ===== 3. 时间筛选边界测试 =====
TEST(FilterFunctional, TimeOnlyNewerThan) {
    FilterRules rules;
    rules.newer_than_sec = 1000000;
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "old";
    e.mtime_sec = 500000;
    EXPECT_FALSE(filter->should_include(e));

    e.mtime_sec = 1000000;
    EXPECT_FALSE(filter->should_include(e));  // 等于边界不算

    e.mtime_sec = 1000001;
    EXPECT_TRUE(filter->should_include(e));
}

TEST(FilterFunctional, TimeOnlyOlderThan) {
    FilterRules rules;
    rules.older_than_sec = 2000000;
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "new";
    e.mtime_sec = 3000000;
    EXPECT_FALSE(filter->should_include(e));

    e.mtime_sec = 2000000;
    EXPECT_FALSE(filter->should_include(e));  // 等于边界不算

    e.mtime_sec = 1999999;
    EXPECT_TRUE(filter->should_include(e));
}

TEST(FilterFunctional, TimeNoRestriction) {
    FilterRules rules;
    rules.newer_than_sec = 0;
    rules.older_than_sec = 0;
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "any_time";
    e.mtime_sec = 0;
    EXPECT_TRUE(filter->should_include(e));

    e.mtime_sec = -1;  // 负数时间戳
    EXPECT_TRUE(filter->should_include(e));
}

// ===== 4. 尺寸筛选边界测试 =====
TEST(FilterFunctional, SizeOnlyMin) {
    FilterRules rules;
    rules.min_size = 1024;
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "small";
    e.size = 1023;
    EXPECT_FALSE(filter->should_include(e));

    e.size = 1024;
    EXPECT_TRUE(filter->should_include(e));  // 等于边界算通过

    e.size = 99999;
    EXPECT_TRUE(filter->should_include(e));
}

TEST(FilterFunctional, SizeOnlyMax) {
    FilterRules rules;
    rules.max_size = 65536;
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "large";
    e.size = 65537;
    EXPECT_FALSE(filter->should_include(e));

    e.size = 65536;
    EXPECT_TRUE(filter->should_include(e));  // 等于边界算通过

    e.size = 0;
    EXPECT_TRUE(filter->should_include(e));
}

TEST(FilterFunctional, SizeZeroMinMax) {
    FilterRules rules;
    rules.min_size = 0;
    rules.max_size = 0;
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "zero";
    e.size = 0;
    EXPECT_TRUE(filter->should_include(e));

    e.size = 9999999;
    EXPECT_TRUE(filter->should_include(e));
}

// ===== 5. 用户筛选边界测试 =====
TEST(FilterFunctional, UserSingleUID) {
    FilterRules rules;
    rules.include_uids = {0};  // root
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "root_file";
    e.uid = 0;
    EXPECT_TRUE(filter->should_include(e));

    e.uid = 1;
    EXPECT_FALSE(filter->should_include(e));
}

TEST(FilterFunctional, UserEmptyUIDList) {
    FilterRules rules;
    rules.include_uids = {};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "any_user";
    e.uid = 0;
    EXPECT_TRUE(filter->should_include(e));

    e.uid = 65534;
    EXPECT_TRUE(filter->should_include(e));
}

// ===== 6. 类型筛选边界测试 =====
TEST(FilterFunctional, TypeAllTypes) {
    FilterRules rules;
    rules.include_types = {EntryType::REGULAR_FILE, EntryType::DIRECTORY,
                           EntryType::SYMBOLIC_LINK, EntryType::HARD_LINK,
                           EntryType::FIFO, EntryType::CHARACTER_DEVICE,
                           EntryType::BLOCK_DEVICE};
    auto filter = create_filter(rules);

    auto test_type = [&](EntryType type, const char* name) {
        EntryInfo e;
        e.path = name;
        e.type = type;
        EXPECT_TRUE(filter->should_include(e));
    };

    test_type(EntryType::REGULAR_FILE, "f.txt");
    test_type(EntryType::DIRECTORY, "d");
    test_type(EntryType::SYMBOLIC_LINK, "sl");
    test_type(EntryType::HARD_LINK, "hl");
    test_type(EntryType::FIFO, "fifo");
    test_type(EntryType::CHARACTER_DEVICE, "cdev");
    test_type(EntryType::BLOCK_DEVICE, "bdev");
}

// ===== 7. 组合场景 — 真实备份场景模拟 =====
TEST(FilterFunctional, BackupSourceCodeOnly) {
    // 场景：只备份 src/ 和 include/ 下的 .cpp/.h 文件，大于 10 字节
    FilterRules rules;
    rules.include_paths = {"src/", "include/"};
    rules.include_names = {"*.cpp", "*.h"};
    rules.min_size = 10;
    auto filter = create_filter(rules);

    EntryInfo e1;
    e1.path = "src/main.cpp";
    e1.type = EntryType::REGULAR_FILE;
    e1.size = 5000;
    EXPECT_TRUE(filter->should_include(e1));

    EntryInfo e2;
    e2.path = "include/utils.h";
    e2.type = EntryType::REGULAR_FILE;
    e2.size = 200;
    EXPECT_TRUE(filter->should_include(e2));

    // 路径不对
    EntryInfo e3;
    e3.path = "build/main.cpp";
    e3.type = EntryType::REGULAR_FILE;
    e3.size = 5000;
    EXPECT_FALSE(filter->should_include(e3));

    // 名称不对
    EntryInfo e4;
    e4.path = "src/README.md";
    e4.type = EntryType::REGULAR_FILE;
    e4.size = 5000;
    EXPECT_FALSE(filter->should_include(e4));

    // 尺寸太小
    EntryInfo e5;
    e5.path = "src/tiny.cpp";
    e5.type = EntryType::REGULAR_FILE;
    e5.size = 5;
    EXPECT_FALSE(filter->should_include(e5));
}

TEST(FilterFunctional, BackupWithFullExclusion) {
    // 场景：排除所有不想备份的目录和文件类型
    FilterRules rules;
    rules.exclude_paths = {"build/", ".git/", "node_modules/", "cache/"};
    rules.exclude_names = {"*.o", "*.tmp", "*.log", "*.cache"};
    auto filter = create_filter(rules);

    // 应该包含的
    EntryInfo good;
    good.path = "src/main.cpp";
    EXPECT_TRUE(filter->should_include(good));

    // 应该排除的 - 路径
    EntryInfo bad_path;
    bad_path.path = "build/output.o";
    EXPECT_FALSE(filter->should_include(bad_path));

    bad_path.path = ".git/config";
    EXPECT_FALSE(filter->should_include(bad_path));

    // 应该排除的 - 名称
    EntryInfo bad_name;
    bad_name.path = "src/debug.log";
    EXPECT_FALSE(filter->should_include(bad_name));

    bad_name.path = "src/obj/file.o";
    EXPECT_FALSE(filter->should_include(bad_name));
}

TEST(FilterFunctional, BackupOnlyLargeFiles) {
    // 场景：只备份大于 1MB 的文件（某些特定类型）
    FilterRules rules;
    rules.include_types = {EntryType::REGULAR_FILE};
    rules.min_size = 1048576;  // 1MB
    rules.exclude_names = {"*.tmp"};
    auto filter = create_filter(rules);

    EntryInfo small;
    small.path = "small.txt";
    small.type = EntryType::REGULAR_FILE;
    small.size = 100;
    EXPECT_FALSE(filter->should_include(small));

    EntryInfo large_good;
    large_good.path = "video.mp4";
    large_good.type = EntryType::REGULAR_FILE;
    large_good.size = 1048576;
    EXPECT_TRUE(filter->should_include(large_good));

    // 临时文件即使大也要排除
    EntryInfo large_tmp;
    large_tmp.path = "temp.tmp";
    large_tmp.type = EntryType::REGULAR_FILE;
    large_tmp.size = 9999999;
    EXPECT_FALSE(filter->should_include(large_tmp));
}

TEST(FilterFunctional, BackupRecentUserFiles) {
    // 场景：只备份用户 uid=1000 最近修改的文件
    FilterRules rules;
    rules.include_uids = {1000};
    rules.newer_than_sec = 1000000;
    auto filter = create_filter(rules);

    EntryInfo recent;
    recent.path = "my_doc.txt";
    recent.uid = 1000;
    recent.mtime_sec = 2000000;
    EXPECT_TRUE(filter->should_include(recent));

    EntryInfo old;
    old.path = "old_doc.txt";
    old.uid = 1000;
    old.mtime_sec = 500000;
    EXPECT_FALSE(filter->should_include(old));

    EntryInfo other_user;
    other_user.path = "other_doc.txt";
    other_user.uid = 2000;
    other_user.mtime_sec = 2000000;
    EXPECT_FALSE(filter->should_include(other_user));
}

// ===== 8. 名称排除 + 路径包含组合 =====
TEST(FilterFunctional, IncludePathButExcludeName) {
    FilterRules rules;
    rules.include_paths = {"docs/"};
    rules.exclude_names = {"*.bak", "*.swp"};
    auto filter = create_filter(rules);

    EntryInfo good;
    good.path = "docs/report.md";
    EXPECT_TRUE(filter->should_include(good));

    EntryInfo bad;
    bad.path = "docs/backup.bak";
    EXPECT_FALSE(filter->should_include(bad));

    bad.path = "docs/.file.swp";
    EXPECT_FALSE(filter->should_include(bad));
}

// ===== 9. 多层次路径排除 =====
TEST(FilterFunctional, MultipleDepthPathExclude) {
    FilterRules rules;
    rules.include_paths = {"home/"};
    rules.exclude_paths = {"home/user/cache/", "home/user/temp/"};
    auto filter = create_filter(rules);

    EntryInfo e;
    e.path = "home/user/docs/report.pdf";
    EXPECT_TRUE(filter->should_include(e));

    e.path = "home/user/cache/data.bin";
    EXPECT_FALSE(filter->should_include(e));

    e.path = "home/user/temp/tmp.txt";
    EXPECT_FALSE(filter->should_include(e));
}
