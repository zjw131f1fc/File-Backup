#include <gtest/gtest.h>
#include "modules/archive_reader/archive_reader.h"
#include "modules/archive_writer/archive_writer.h"
#include "../../tests/helpers/temp_dir.h"
#include <sstream>

using namespace backup;
using namespace backup::testing;

// 辅助：创建一个包含若干条目的有效归档
static std::string create_valid_archive(TempDir& tmp) {
    auto path = tmp.path() + "/archive.dat";
    auto writer = create_archive(path);

    EntryInfo dir_entry;
    dir_entry.path = "dir";
    dir_entry.type = EntryType::DIRECTORY;
    writer->add_entry(dir_entry);

    EntryInfo file_entry;
    file_entry.path = "dir/file.txt";
    file_entry.type = EntryType::REGULAR_FILE;
    file_entry.size = 5;
    std::istringstream content("hello");
    writer->add_entry(file_entry, content);

    writer->commit();
    return path;
}

// ===== 1. 正确归档 validate 通过 =====
TEST(ArchiveReaderContract, ValidateCorrectArchive) {
    TempDir tmp;
    std::string path = create_valid_archive(tmp);

    auto reader = open_archive(path);
    EXPECT_NE(reader, nullptr);
    Result r = reader->validate();
    EXPECT_EQ(r.status, Status::SUCCESS);
}

TEST(ArchiveReaderContract, ValidateMissingArchiveFails) {
    TempDir tmp;
    auto reader = open_archive(tmp.path() + "/missing.dat");

    EXPECT_NE(reader, nullptr);
    Result r = reader->validate();
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 2. 格式标识错误 =====
TEST(ArchiveReaderContract, ValidateBadFormat) {
    TempDir tmp;
    // 写入一个非归档格式的文件
    std::string path = tmp.create_file("bad.dat", "this is not an archive");

    auto reader = open_archive(path);
    EXPECT_NE(reader, nullptr);
    Result r = reader->validate();
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 3. 截断归档 =====
TEST(ArchiveReaderContract, ValidateTruncatedArchive) {
    TempDir tmp;
    std::string path = create_valid_archive(tmp);

    // 截断归档文件：只保留前半部分
    auto file_size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, file_size / 2);

    auto reader = open_archive(path);
    Result r = reader->validate();
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 4. 危险路径 =====
TEST(ArchiveReaderContract, ValidateDangerousPath) {
    TempDir tmp;
    auto path = tmp.path() + "/danger.dat";
    auto writer = create_archive(path);

    EntryInfo abs_entry;
    abs_entry.path = "/etc/passwd";  // 绝对路径
    abs_entry.type = EntryType::REGULAR_FILE;
    writer->add_entry(abs_entry);

    EntryInfo traversal_entry;
    traversal_entry.path = "../../etc/shadow";  // 路径穿越
    traversal_entry.type = EntryType::REGULAR_FILE;
    writer->add_entry(traversal_entry);

    writer->commit();

    auto reader = open_archive(path);
    Result r = reader->validate();
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 5. 遍历所有条目 =====
TEST(ArchiveReaderContract, EnumerateAllEntries) {
    TempDir tmp;
    std::string path = create_valid_archive(tmp);

    auto reader = open_archive(path);
    reader->validate();

    int count = 0;
    while (reader->has_next_entry()) {
        EntryInfo info;
        Result r = reader->next_entry(info);
        EXPECT_EQ(r.status, Status::SUCCESS);
        count++;
    }
    EXPECT_EQ(count, 2);  // dir + file.txt
}

// ===== 6. 普通文件 open_content 返回流 =====
TEST(ArchiveReaderContract, OpenContentForRegularFile) {
    TempDir tmp;
    std::string path = create_valid_archive(tmp);

    auto reader = open_archive(path);
    reader->validate();

    EntryInfo dir_info;
    reader->next_entry(dir_info);
    EXPECT_EQ(dir_info.type, EntryType::DIRECTORY);
    auto null_stream = reader->open_content(dir_info);
    EXPECT_EQ(null_stream, nullptr);

    EntryInfo file_info;
    reader->next_entry(file_info);
    EXPECT_EQ(file_info.type, EntryType::REGULAR_FILE);
    auto stream = reader->open_content(file_info);
    if (!stream) {
        // 桩实现返回 nullptr，真实实现应返回有效流
        EXPECT_NE(stream, nullptr);
        return;
    }

    std::ostringstream oss;
    oss << stream->rdbuf();
    EXPECT_EQ(oss.str(), "hello");
}

// ===== 7. 非文件条目 open_content 返回 nullptr =====
TEST(ArchiveReaderContract, OpenContentForNonFile) {
    TempDir tmp;
    auto path = tmp.path() + "/archive.dat";
    auto writer = create_archive(path);

    EntryInfo dir_entry;
    dir_entry.path = "mydir";
    dir_entry.type = EntryType::DIRECTORY;
    writer->add_entry(dir_entry);

    EntryInfo link_entry;
    link_entry.path = "mylink";
    link_entry.type = EntryType::SYMBOLIC_LINK;
    link_entry.link_target = "mydir";
    writer->add_entry(link_entry);

    writer->commit();

    auto reader = open_archive(path);
    reader->validate();

    EntryInfo e1;
    reader->next_entry(e1);
    EXPECT_EQ(reader->open_content(e1), nullptr);

    EntryInfo e2;
    reader->next_entry(e2);
    EXPECT_EQ(reader->open_content(e2), nullptr);
}
