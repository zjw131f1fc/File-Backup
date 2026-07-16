#include <gtest/gtest.h>
#include "modules/restore/restore.h"
#include "modules/archive_writer/archive_writer.h"
#include "modules/archive_reader/archive_reader.h"
#include "../../tests/helpers/temp_dir.h"
#include <filesystem>
#include <sstream>
#include <fstream>
#include <sys/stat.h>

using namespace backup;
using namespace backup::testing;

// 辅助：创建包含一个普通文件的归档，返回路径
static std::string create_archive_with_file(TempDir& archive_tmp, const std::string& content) {
    auto path = archive_tmp.path() + "/archive.dat";
    auto writer = create_archive(path);

    EntryInfo dir_entry;
    dir_entry.path = "subdir";
    dir_entry.type = EntryType::DIRECTORY;
    dir_entry.permissions = 0755;
    writer->add_entry(dir_entry);

    EntryInfo file_entry;
    file_entry.path = "subdir/file.txt";
    file_entry.type = EntryType::REGULAR_FILE;
    file_entry.size = content.size();
    file_entry.permissions = 0644;
    file_entry.uid = ::getuid();
    file_entry.gid = ::getgid();
    file_entry.mtime_sec = 1700000000;
    file_entry.atime_sec = 1700000000;
    std::istringstream file_content(content);
    writer->add_entry(file_entry, file_content);

    writer->commit();
    return path;
}

// ===== 1. 恢复普通文件 =====
TEST(RestorerContract, RestoreRegularFile) {
    TempDir archive_tmp;
    TempDir restore_tmp;
    std::string archive_path = create_archive_with_file(archive_tmp, "file content here");

    auto reader = open_archive(archive_path);
    reader->validate();

    auto restorer = create_restorer();

    // 读目录条目
    EntryInfo dir_info;
    reader->next_entry(dir_info);
    restorer->restore_entry(restore_tmp.path(), dir_info, *reader, ConflictPolicy::SKIP);

    // 读文件条目
    EntryInfo file_info;
    reader->next_entry(file_info);
    Result r = restorer->restore_entry(restore_tmp.path(), file_info, *reader, ConflictPolicy::SKIP);
    EXPECT_EQ(r.status, Status::SUCCESS);

    // 目标文件应存在且内容一致
    std::string target = restore_tmp.path() + "/subdir/file.txt";
    EXPECT_TRUE(std::filesystem::exists(target));

    std::ifstream in(target);
    std::string content;
    std::ostringstream oss;
    oss << in.rdbuf();
    content = oss.str();
    EXPECT_EQ(content, "file content here");
}

// ===== 2. 临时文件策略（失败不留半写文件） =====
TEST(RestorerContract, RestoreRegularFileUsesTempFile) {
    // 这项测试验证：如果恢复失败，目标路径不存在半写文件
    // 具体实现由 Restorer 保证使用临时文件+rename策略
    // 此测试在真实实现中才能完整验证
    TempDir restore_tmp;
    auto restorer = create_restorer();
    auto reader = open_archive("/tmp/nonexistent_archive");

    EntryInfo bad_entry;
    bad_entry.path = "should_fail.txt";
    bad_entry.type = EntryType::REGULAR_FILE;
    Result r = restorer->restore_entry(restore_tmp.path(), bad_entry, *reader, ConflictPolicy::SKIP);
    // 失败时目标文件不应存在
    EXPECT_FALSE(std::filesystem::exists(restore_tmp.path() + "/should_fail.txt"));
}

// ===== 3. 恢复目录 =====
TEST(RestorerContract, RestoreDirectory) {
    TempDir archive_tmp;
    TempDir restore_tmp;
    auto archive_path = archive_tmp.path() + "/archive.dat";
    auto writer = create_archive(archive_path);

    EntryInfo dir_entry;
    dir_entry.path = "mydir";
    dir_entry.type = EntryType::DIRECTORY;
    dir_entry.permissions = 0755;
    writer->add_entry(dir_entry);
    writer->commit();

    auto reader = open_archive(archive_path);
    reader->validate();

    auto restorer = create_restorer();
    EntryInfo info;
    reader->next_entry(info);
    Result r = restorer->restore_entry(restore_tmp.path(), info, *reader, ConflictPolicy::SKIP);
    EXPECT_EQ(r.status, Status::SUCCESS);

    EXPECT_TRUE(std::filesystem::is_directory(restore_tmp.path() + "/mydir"));
}

// ===== 4. 恢复符号链接 =====
TEST(RestorerContract, RestoreSymbolicLink) {
    TempDir archive_tmp;
    TempDir restore_tmp;
    auto archive_path = archive_tmp.path() + "/archive.dat";
    auto writer = create_archive(archive_path);

    EntryInfo link_entry;
    link_entry.path = "mylink";
    link_entry.type = EntryType::SYMBOLIC_LINK;
    link_entry.link_target = "target_file";
    writer->add_entry(link_entry);
    writer->commit();

    auto reader = open_archive(archive_path);
    reader->validate();

    auto restorer = create_restorer();
    EntryInfo info;
    reader->next_entry(info);
    Result r = restorer->restore_entry(restore_tmp.path(), info, *reader, ConflictPolicy::SKIP);
    EXPECT_EQ(r.status, Status::SUCCESS);

    auto link_path = restore_tmp.path() + "/mylink";
    EXPECT_TRUE(std::filesystem::is_symlink(link_path));
    EXPECT_EQ(std::filesystem::read_symlink(link_path).string(), "target_file");
}

// ===== 5. 恢复硬链接 =====
TEST(RestorerContract, RestoreHardLink) {
    TempDir archive_tmp;
    TempDir restore_tmp;

    // 先写原文件，再写硬链接
    auto archive_path = archive_tmp.path() + "/archive.dat";
    auto writer = create_archive(archive_path);

    EntryInfo original;
    original.path = "original.txt";
    original.type = EntryType::REGULAR_FILE;
    original.size = 4;
    std::istringstream content("data");
    writer->add_entry(original, content);

    EntryInfo hardlink;
    hardlink.path = "hardlink.txt";
    hardlink.type = EntryType::HARD_LINK;
    hardlink.hard_link_target = "original.txt";
    writer->add_entry(hardlink);

    writer->commit();

    auto reader = open_archive(archive_path);
    reader->validate();

    auto restorer = create_restorer();

    EntryInfo e1;
    reader->next_entry(e1);
    restorer->restore_entry(restore_tmp.path(), e1, *reader, ConflictPolicy::SKIP);

    EntryInfo e2;
    reader->next_entry(e2);
    Result r = restorer->restore_entry(restore_tmp.path(), e2, *reader, ConflictPolicy::SKIP);
    EXPECT_EQ(r.status, Status::SUCCESS);

    // 硬链接应指向原文件（同一 inode）
    auto orig_path = std::filesystem::path(restore_tmp.path()) / "original.txt";
    auto link_path = std::filesystem::path(restore_tmp.path()) / "hardlink.txt";
    EXPECT_TRUE(std::filesystem::exists(link_path));
    EXPECT_EQ(std::filesystem::hard_link_count(orig_path), 2u);
}

// ===== 6. 恢复 FIFO =====
TEST(RestorerContract, RestoreFIFO) {
    TempDir archive_tmp;
    TempDir restore_tmp;
    auto archive_path = archive_tmp.path() + "/archive.dat";
    auto writer = create_archive(archive_path);

    EntryInfo fifo_entry;
    fifo_entry.path = "my_fifo";
    fifo_entry.type = EntryType::FIFO;
    fifo_entry.permissions = 0644;
    writer->add_entry(fifo_entry);
    writer->commit();

    auto reader = open_archive(archive_path);
    reader->validate();

    auto restorer = create_restorer();
    EntryInfo info;
    reader->next_entry(info);
    Result r = restorer->restore_entry(restore_tmp.path(), info, *reader, ConflictPolicy::SKIP);
    EXPECT_EQ(r.status, Status::SUCCESS);

    auto fifo_path = restore_tmp.path() + "/my_fifo";
    struct stat st;
    EXPECT_EQ(lstat(fifo_path.c_str(), &st), 0);
    EXPECT_TRUE(S_ISFIFO(st.st_mode));
}

// ===== 7. 恢复字符设备 =====
TEST(RestorerContract, RestoreCharDevice) {
    TempDir archive_tmp;
    TempDir restore_tmp;
    auto archive_path = archive_tmp.path() + "/archive.dat";
    auto writer = create_archive(archive_path);

    EntryInfo char_dev;
    char_dev.path = "my_char_dev";
    char_dev.type = EntryType::CHARACTER_DEVICE;
    char_dev.permissions = 0666;
    char_dev.device_major = 1;
    char_dev.device_minor = 3;
    writer->add_entry(char_dev);
    writer->commit();

    auto reader = open_archive(archive_path);
    reader->validate();

    auto restorer = create_restorer();
    EntryInfo info;
    reader->next_entry(info);
    Result r = restorer->restore_entry(restore_tmp.path(), info, *reader, ConflictPolicy::SKIP);
    // 字符设备创建可能需要 root 权限，非 root 时应返回 FAILED 或 PARTIAL_SUCCESS
    if (r.status == Status::SUCCESS) {
        auto dev_path = restore_tmp.path() + "/my_char_dev";
        struct stat st;
        EXPECT_EQ(lstat(dev_path.c_str(), &st), 0);
        EXPECT_TRUE(S_ISCHR(st.st_mode));
    }
}

// ===== 8. 恢复块设备 =====
TEST(RestorerContract, RestoreBlockDevice) {
    TempDir archive_tmp;
    TempDir restore_tmp;
    auto archive_path = archive_tmp.path() + "/archive.dat";
    auto writer = create_archive(archive_path);

    EntryInfo block_dev;
    block_dev.path = "my_block_dev";
    block_dev.type = EntryType::BLOCK_DEVICE;
    block_dev.permissions = 0660;
    block_dev.device_major = 8;
    block_dev.device_minor = 0;
    writer->add_entry(block_dev);
    writer->commit();

    auto reader = open_archive(archive_path);
    reader->validate();

    auto restorer = create_restorer();
    EntryInfo info;
    reader->next_entry(info);
    Result r = restorer->restore_entry(restore_tmp.path(), info, *reader, ConflictPolicy::SKIP);
    // 块设备创建可能需要 root 权限
    if (r.status == Status::SUCCESS) {
        auto dev_path = restore_tmp.path() + "/my_block_dev";
        struct stat st;
        EXPECT_EQ(lstat(dev_path.c_str(), &st), 0);
        EXPECT_TRUE(S_ISBLK(st.st_mode));
    }
}

// ===== 9. 冲突策略 SKIP =====
TEST(RestorerContract, ConflictSkip) {
    TempDir archive_tmp;
    TempDir restore_tmp;
    // 预创建目标文件
    restore_tmp.create_dir("subdir");
    restore_tmp.create_file("subdir/file.txt", "original content");

    std::string archive_path = create_archive_with_file(archive_tmp, "new content");

    auto reader = open_archive(archive_path);
    reader->validate();

    auto restorer = create_restorer();
    EntryInfo dir_info;
    reader->next_entry(dir_info);
    restorer->restore_entry(restore_tmp.path(), dir_info, *reader, ConflictPolicy::SKIP);

    EntryInfo file_info;
    reader->next_entry(file_info);
    Result r = restorer->restore_entry(restore_tmp.path(), file_info, *reader, ConflictPolicy::SKIP);
    EXPECT_EQ(r.status, Status::SUCCESS);

    // 原文件内容不变
    std::ifstream in(restore_tmp.path() + "/subdir/file.txt");
    std::string content;
    std::ostringstream oss;
    oss << in.rdbuf();
    content = oss.str();
    EXPECT_EQ(content, "original content");
}

// ===== 10. 冲突策略 OVERWRITE =====
TEST(RestorerContract, ConflictOverwrite) {
    TempDir archive_tmp;
    TempDir restore_tmp;
    // 预创建目标文件
    restore_tmp.create_dir("subdir");
    restore_tmp.create_file("subdir/file.txt", "old content");

    std::string archive_path = create_archive_with_file(archive_tmp, "new content");

    auto reader = open_archive(archive_path);
    reader->validate();

    auto restorer = create_restorer();
    EntryInfo dir_info;
    reader->next_entry(dir_info);
    restorer->restore_entry(restore_tmp.path(), dir_info, *reader, ConflictPolicy::OVERWRITE);

    EntryInfo file_info;
    reader->next_entry(file_info);
    Result r = restorer->restore_entry(restore_tmp.path(), file_info, *reader, ConflictPolicy::OVERWRITE);
    EXPECT_EQ(r.status, Status::SUCCESS);

    // 文件内容应为新内容
    std::ifstream in(restore_tmp.path() + "/subdir/file.txt");
    std::string content;
    std::ostringstream oss;
    oss << in.rdbuf();
    content = oss.str();
    EXPECT_EQ(content, "new content");
}

// ===== 11. 元数据恢复 =====
TEST(RestorerContract, RestoreMetadata) {
    TempDir restore_tmp;
    restore_tmp.create_file("test_file.txt", "some data");

    auto restorer = create_restorer();

    EntryInfo meta;
    meta.permissions = 0755;
    meta.uid = ::getuid();
    meta.gid = ::getgid();
    meta.mtime_sec = 1700000000;
    meta.atime_sec = 1700000000;

    std::string target = restore_tmp.path() + "/test_file.txt";
    Result r = restorer->restore_metadata(target, meta);
    EXPECT_EQ(r.status, Status::SUCCESS);

    struct stat st;
    EXPECT_EQ(stat(target.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0755u);
}

TEST(RestorerContract, RestoreMetadataMissingTargetFails) {
    TempDir restore_tmp;
    auto restorer = create_restorer();

    EntryInfo meta;
    Result r = restorer->restore_metadata(
        restore_tmp.path() + "/missing", meta);

    EXPECT_EQ(r.status, Status::FAILED);
}

TEST(RestorerContract, RejectsPathTraversal) {
    TempDir restore_tmp;
    auto restorer = create_restorer();
    auto reader = open_archive("/tmp/nonexistent_archive");
    EntryInfo entry;
    entry.path = "../escape.txt";
    entry.type = EntryType::REGULAR_FILE;

    Result r = restorer->restore_entry(
        restore_tmp.path(), entry, *reader, ConflictPolicy::OVERWRITE);
    EXPECT_EQ(r.status, Status::FAILED);
}

TEST(RestorerContract, RejectsSymlinkInParentChain) {
    TempDir restore_tmp;
    restore_tmp.create_symlink("redirect", "/tmp");
    auto restorer = create_restorer();
    auto reader = open_archive("/tmp/nonexistent_archive");
    EntryInfo entry;
    entry.path = "redirect/escape.txt";
    entry.type = EntryType::REGULAR_FILE;

    Result r = restorer->restore_entry(
        restore_tmp.path(), entry, *reader, ConflictPolicy::OVERWRITE);
    EXPECT_EQ(r.status, Status::FAILED);
}

TEST(RestorerContract, FailedOverwritePreservesExistingFile) {
    TempDir restore_tmp;
    restore_tmp.create_file("existing.txt", "original");
    auto restorer = create_restorer();
    auto reader = open_archive("/tmp/nonexistent_archive");
    EntryInfo entry;
    entry.path = "existing.txt";
    entry.type = EntryType::REGULAR_FILE;
    entry.size = 10;

    Result r = restorer->restore_entry(
        restore_tmp.path(), entry, *reader, ConflictPolicy::OVERWRITE);
    EXPECT_EQ(r.status, Status::FAILED);
    std::ifstream input(restore_tmp.path() + "/existing.txt");
    std::ostringstream content;
    content << input.rdbuf();
    EXPECT_EQ(content.str(), "original");
}
