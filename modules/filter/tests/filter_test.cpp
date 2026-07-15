#include <gtest/gtest.h>
#include "modules/filter/filter.h"

using namespace backup;

// ===== 1. 空规则包含所有 =====
TEST(FilterContract, EmptyRulesIncludesAll) {
    FilterRules rules;  // 全部默认为空
    auto filter = create_filter(rules);

    EntryInfo entry;
    entry.path = "any/path/file.txt";
    entry.type = EntryType::REGULAR_FILE;
    entry.size = 1024;
    entry.uid = 1000;
    entry.mtime_sec = 1000000;

    EXPECT_TRUE(filter->should_include(entry));
}

// ===== 2. 路径筛选 =====
TEST(FilterContract, FilterByPathInclude) {
    FilterRules rules;
    rules.include_paths = {"src/", "lib/"};
    auto filter = create_filter(rules);

    EntryInfo e1;
    e1.path = "src/main.cpp";
    EXPECT_TRUE(filter->should_include(e1));

    EntryInfo e2;
    e2.path = "lib/utils.h";
    EXPECT_TRUE(filter->should_include(e2));

    EntryInfo e3;
    e3.path = "build/output.o";
    EXPECT_FALSE(filter->should_include(e3));
}

TEST(FilterContract, FilterByPathExclude) {
    FilterRules rules;
    rules.exclude_paths = {"build/", ".git/"};
    auto filter = create_filter(rules);

    EntryInfo e1;
    e1.path = "build/output.o";
    EXPECT_FALSE(filter->should_include(e1));

    EntryInfo e2;
    e2.path = ".git/config";
    EXPECT_FALSE(filter->should_include(e2));
}

TEST(FilterContract, FilterByPathExcludeOverridesInclude) {
    FilterRules rules;
    rules.include_paths = {"src/"};
    rules.exclude_paths = {"src/test/"};
    auto filter = create_filter(rules);

    EntryInfo e1;
    e1.path = "src/main.cpp";
    EXPECT_TRUE(filter->should_include(e1));

    EntryInfo e2;
    e2.path = "src/test/test_main.cpp";
    EXPECT_FALSE(filter->should_include(e2));
}

// ===== 3. 类型筛选 =====
TEST(FilterContract, FilterByType) {
    FilterRules rules;
    rules.include_types = {EntryType::REGULAR_FILE, EntryType::DIRECTORY};
    auto filter = create_filter(rules);

    EntryInfo file_entry;
    file_entry.path = "file.txt";
    file_entry.type = EntryType::REGULAR_FILE;
    EXPECT_TRUE(filter->should_include(file_entry));

    EntryInfo dir_entry;
    dir_entry.path = "subdir";
    dir_entry.type = EntryType::DIRECTORY;
    EXPECT_TRUE(filter->should_include(dir_entry));

    EntryInfo symlink_entry;
    symlink_entry.path = "link";
    symlink_entry.type = EntryType::SYMBOLIC_LINK;
    EXPECT_FALSE(filter->should_include(symlink_entry));
}

// ===== 4. 名称筛选 =====
TEST(FilterContract, FilterByNameInclude) {
    FilterRules rules;
    rules.include_names = {"*.cpp", "*.h"};
    auto filter = create_filter(rules);

    EntryInfo e1;
    e1.path = "src/main.cpp";
    EXPECT_TRUE(filter->should_include(e1));

    EntryInfo e2;
    e2.path = "include/utils.h";
    EXPECT_TRUE(filter->should_include(e2));

    EntryInfo e3;
    e3.path = "README.md";
    EXPECT_FALSE(filter->should_include(e3));
}

TEST(FilterContract, FilterByNameExclude) {
    FilterRules rules;
    rules.exclude_names = {"*.o", "*.tmp"};
    auto filter = create_filter(rules);

    EntryInfo e1;
    e1.path = "build/output.o";
    EXPECT_FALSE(filter->should_include(e1));

    EntryInfo e2;
    e2.path = "cache/temp.tmp";
    EXPECT_FALSE(filter->should_include(e2));
}

// ===== 5. 时间筛选 =====
TEST(FilterContract, FilterByTime) {
    FilterRules rules;
    rules.newer_than_sec = 2000000;
    rules.older_than_sec = 3000000;
    auto filter = create_filter(rules);

    EntryInfo too_old;
    too_old.path = "old_file";
    too_old.mtime_sec = 1000000;
    EXPECT_FALSE(filter->should_include(too_old));

    EntryInfo in_range;
    in_range.path = "mid_file";
    in_range.mtime_sec = 2500000;
    EXPECT_TRUE(filter->should_include(in_range));

    EntryInfo too_new;
    too_new.path = "new_file";
    too_new.mtime_sec = 4000000;
    EXPECT_FALSE(filter->should_include(too_new));
}

// ===== 6. 尺寸筛选 =====
TEST(FilterContract, FilterBySize) {
    FilterRules rules;
    rules.min_size = 100;
    rules.max_size = 1000;
    auto filter = create_filter(rules);

    EntryInfo too_small;
    too_small.path = "small";
    too_small.size = 50;
    EXPECT_FALSE(filter->should_include(too_small));

    EntryInfo in_range;
    in_range.path = "medium";
    in_range.size = 500;
    EXPECT_TRUE(filter->should_include(in_range));

    EntryInfo too_large;
    too_large.path = "large";
    too_large.size = 5000;
    EXPECT_FALSE(filter->should_include(too_large));
}

// ===== 7. 用户筛选 =====
TEST(FilterContract, FilterByUser) {
    FilterRules rules;
    rules.include_uids = {1000, 1001};
    auto filter = create_filter(rules);

    EntryInfo included;
    included.path = "my_file";
    included.uid = 1000;
    EXPECT_TRUE(filter->should_include(included));

    EntryInfo excluded;
    excluded.path = "other_file";
    excluded.uid = 2000;
    EXPECT_FALSE(filter->should_include(excluded));
}

// ===== 8. 组合筛选（AND 逻辑） =====
TEST(FilterContract, CombinedRulesAllMustPass) {
    FilterRules rules;
    rules.include_types = {EntryType::REGULAR_FILE};
    rules.min_size = 100;
    rules.include_uids = {1000};
    auto filter = create_filter(rules);

    // 满足全部条件
    EntryInfo good;
    good.path = "good.txt";
    good.type = EntryType::REGULAR_FILE;
    good.size = 500;
    good.uid = 1000;
    EXPECT_TRUE(filter->should_include(good));

    // 类型不对
    EntryInfo wrong_type;
    wrong_type.path = "subdir";
    wrong_type.type = EntryType::DIRECTORY;
    wrong_type.size = 500;
    wrong_type.uid = 1000;
    EXPECT_FALSE(filter->should_include(wrong_type));

    // 尺寸不对
    EntryInfo wrong_size;
    wrong_size.path = "tiny";
    wrong_size.type = EntryType::REGULAR_FILE;
    wrong_size.size = 10;
    wrong_size.uid = 1000;
    EXPECT_FALSE(filter->should_include(wrong_size));

    // UID 不对
    EntryInfo wrong_uid;
    wrong_uid.path = "other";
    wrong_uid.type = EntryType::REGULAR_FILE;
    wrong_uid.size = 500;
    wrong_uid.uid = 2000;
    EXPECT_FALSE(filter->should_include(wrong_uid));
}
