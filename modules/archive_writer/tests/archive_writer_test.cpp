#include <gtest/gtest.h>
#include "modules/archive_writer/archive_writer.h"
#include "modules/archive_reader/archive_reader.h"
#include "../../tests/helpers/temp_dir.h"
#include <filesystem>
#include <sstream>

using namespace backup;
using namespace backup::testing;

// ===== 1. commit 创建可读归档 =====
TEST(ArchiveWriterContract, CommitCreatesValidArchive) {
    TempDir tmp;
    auto writer = create_archive(tmp.path() + "/archive.dat");

    EntryInfo dir_entry;
    dir_entry.path = "rootdir";
    dir_entry.type = EntryType::DIRECTORY;
    writer->add_entry(dir_entry);

    Result r = writer->commit();
    EXPECT_EQ(r.status, Status::SUCCESS);

    // 归档文件应存在且可读
    auto reader = open_archive(tmp.path() + "/archive.dat");
    EXPECT_NE(reader, nullptr);
    Result vr = reader->validate();
    EXPECT_EQ(vr.status, Status::SUCCESS);
}

// ===== 2. abort 不留文件 =====
TEST(ArchiveWriterContract, AbortDeletesFile) {
    TempDir tmp;
    std::string archive_path = tmp.path() + "/archive.dat";
    auto writer = create_archive(archive_path);

    EntryInfo entry;
    entry.path = "file.txt";
    entry.type = EntryType::REGULAR_FILE;
    std::istringstream content("data");
    writer->add_entry(entry, content);

    Result r = writer->abort();
    EXPECT_EQ(r.status, Status::SUCCESS);

    // 归档文件不应存在
    EXPECT_FALSE(std::filesystem::exists(archive_path));
}

// ===== 3. 写入普通文件内容完整 =====
TEST(ArchiveWriterContract, WriteRegularFilePreservesContent) {
    TempDir tmp;
    auto writer = create_archive(tmp.path() + "/archive.dat");

    EntryInfo file_entry;
    file_entry.path = "test.txt";
    file_entry.type = EntryType::REGULAR_FILE;
    file_entry.size = 12;
    std::istringstream content("hello world!");
    Result add_r = writer->add_entry(file_entry, content);
    EXPECT_EQ(add_r.status, Status::SUCCESS);

    Result commit_r = writer->commit();
    EXPECT_EQ(commit_r.status, Status::SUCCESS);

    // 读回验证内容
    auto reader = open_archive(tmp.path() + "/archive.dat");
    Result vr = reader->validate();
    EXPECT_EQ(vr.status, Status::SUCCESS);

    EntryInfo read_entry;
    reader->next_entry(read_entry);
    EXPECT_EQ(read_entry.path, "test.txt");
    EXPECT_EQ(read_entry.type, EntryType::REGULAR_FILE);

    auto stream = reader->open_content(read_entry);
    if (!stream) {
        // 桩实现返回 nullptr，真实实现应返回有效流
        EXPECT_NE(stream, nullptr);
        return;
    }

    std::string read_content;
    std::ostringstream oss;
    oss << stream->rdbuf();
    read_content = oss.str();
    EXPECT_EQ(read_content, "hello world!");
}

// ===== 4. 写入目录条目 =====
TEST(ArchiveWriterContract, WriteDirectoryEntry) {
    TempDir tmp;
    auto writer = create_archive(tmp.path() + "/archive.dat");

    EntryInfo dir_entry;
    dir_entry.path = "mydir";
    dir_entry.type = EntryType::DIRECTORY;
    dir_entry.permissions = 0755;
    Result r = writer->add_entry(dir_entry);
    EXPECT_EQ(r.status, Status::SUCCESS);

    writer->commit();

    auto reader = open_archive(tmp.path() + "/archive.dat");
    reader->validate();

    EntryInfo read_entry;
    reader->next_entry(read_entry);
    EXPECT_EQ(read_entry.type, EntryType::DIRECTORY);
    EXPECT_EQ(read_entry.path, "mydir");
}

// ===== 5. 写入符号链接条目 =====
TEST(ArchiveWriterContract, WriteSymlinkEntry) {
    TempDir tmp;
    auto writer = create_archive(tmp.path() + "/archive.dat");

    EntryInfo link_entry;
    link_entry.path = "link";
    link_entry.type = EntryType::SYMBOLIC_LINK;
    link_entry.link_target = "target_file";
    Result r = writer->add_entry(link_entry);
    EXPECT_EQ(r.status, Status::SUCCESS);

    writer->commit();

    auto reader = open_archive(tmp.path() + "/archive.dat");
    reader->validate();

    EntryInfo read_entry;
    reader->next_entry(read_entry);
    EXPECT_EQ(read_entry.type, EntryType::SYMBOLIC_LINK);
    EXPECT_EQ(read_entry.link_target, "target_file");
}

// ===== 6. 写入硬链接条目 =====
TEST(ArchiveWriterContract, WriteHardLinkEntry) {
    TempDir tmp;
    auto writer = create_archive(tmp.path() + "/archive.dat");

    EntryInfo link_entry;
    link_entry.path = "hardlink.txt";
    link_entry.type = EntryType::HARD_LINK;
    link_entry.hard_link_target = "original.txt";
    Result r = writer->add_entry(link_entry);
    EXPECT_EQ(r.status, Status::SUCCESS);

    writer->commit();

    auto reader = open_archive(tmp.path() + "/archive.dat");
    reader->validate();

    EntryInfo read_entry;
    reader->next_entry(read_entry);
    EXPECT_EQ(read_entry.type, EntryType::HARD_LINK);
    EXPECT_EQ(read_entry.hard_link_target, "original.txt");
}

// ===== 7. 写入特殊文件条目 =====
TEST(ArchiveWriterContract, WriteSpecialFileEntry) {
    TempDir tmp;
    auto writer = create_archive(tmp.path() + "/archive.dat");

    EntryInfo fifo_entry;
    fifo_entry.path = "my_fifo";
    fifo_entry.type = EntryType::FIFO;
    fifo_entry.permissions = 0644;
    writer->add_entry(fifo_entry);

    EntryInfo char_dev;
    char_dev.path = "ttyS0";
    char_dev.type = EntryType::CHARACTER_DEVICE;
    char_dev.permissions = 0666;
    char_dev.device_major = 4;
    char_dev.device_minor = 64;
    writer->add_entry(char_dev);

    writer->commit();

    auto reader = open_archive(tmp.path() + "/archive.dat");
    reader->validate();

    EntryInfo e1;
    reader->next_entry(e1);
    EXPECT_EQ(e1.type, EntryType::FIFO);

    EntryInfo e2;
    reader->next_entry(e2);
    EXPECT_EQ(e2.type, EntryType::CHARACTER_DEVICE);
    EXPECT_EQ(e2.device_major, 4u);
    EXPECT_EQ(e2.device_minor, 64u);
}

// ===== 8. commit/abort 后不可再用 =====
TEST(ArchiveWriterContract, AddEntryAfterCommitFails) {
    TempDir tmp;
    auto writer = create_archive(tmp.path() + "/archive.dat");

    writer->commit();

    EntryInfo entry;
    entry.path = "late.txt";
    entry.type = EntryType::REGULAR_FILE;
    Result r = writer->add_entry(entry);
    EXPECT_EQ(r.status, Status::FAILED);
}

TEST(ArchiveWriterContract, AddEntryAfterAbortFails) {
    TempDir tmp;
    auto writer = create_archive(tmp.path() + "/archive.dat");

    writer->abort();

    EntryInfo entry;
    entry.path = "late.txt";
    entry.type = EntryType::REGULAR_FILE;
    Result r = writer->add_entry(entry);
    EXPECT_EQ(r.status, Status::FAILED);
}

TEST(ArchiveWriterContract, AbortAfterCommitFailsAndPreservesOutput) {
    TempDir tmp;
    const std::string path = tmp.create_file("archive.dat", "existing");

    auto writer = create_archive(path);
    EXPECT_EQ(writer->abort().status, Status::SUCCESS);
    EXPECT_TRUE(std::filesystem::exists(path));

    auto committed = create_archive(path);
    EXPECT_EQ(committed->commit().status, Status::SUCCESS);
    EXPECT_EQ(committed->abort().status, Status::FAILED);
    EXPECT_TRUE(std::filesystem::exists(path));
}
