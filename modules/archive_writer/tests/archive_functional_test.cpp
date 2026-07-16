#include <gtest/gtest.h>
#include "modules/archive_writer/archive_writer.h"
#include "modules/archive_reader/archive_reader.h"
#include "../../tests/helpers/temp_dir.h"
#include <sstream>
#include <filesystem>

using namespace backup;
using namespace backup::testing;

// ============================================================
// 归档模块 — 功能测试
// 覆盖契约测试之外的边界条件、组合场景
// ============================================================

// ===== 1. 空归档（无条目）=========
TEST(ArchiveFunctional, EmptyArchiveIsValid) {
    TempDir tmp;
    auto path = tmp.path() + "/empty.dat";
    {
        auto writer = create_archive(path);
        auto r = writer->commit();
        EXPECT_EQ(r.status, Status::SUCCESS);
    }

    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0);

    auto reader = open_archive(path);
    auto r = reader->validate();
    EXPECT_EQ(r.status, Status::SUCCESS);
    EXPECT_FALSE(reader->has_next_entry());
}

// ===== 2. 大量条目枚举 =====
TEST(ArchiveFunctional, ManyEntries) {
    TempDir tmp;
    auto path = tmp.path() + "/many.dat";
    const int N = 100;
    {
        auto writer = create_archive(path);
        for (int i = 0; i < N; i++) {
            EntryInfo e;
            e.path = "file_" + std::to_string(i) + ".txt";
            e.type = EntryType::REGULAR_FILE;
            e.size = 4;
            std::istringstream content("data");
            auto r = writer->add_entry(e, content);
            EXPECT_EQ(r.status, Status::SUCCESS);
        }
        writer->commit();
    }

    auto reader = open_archive(path);
    reader->validate();

    int count = 0;
    while (reader->has_next_entry()) {
        EntryInfo info;
        reader->next_entry(info);
        EXPECT_EQ(info.type, EntryType::REGULAR_FILE);
        count++;
    }
    EXPECT_EQ(count, N);
}

// ===== 3. 大文件内容完整性 =====
TEST(ArchiveFunctional, LargeFileContent) {
    TempDir tmp;
    auto path = tmp.path() + "/large.dat";

    // 生成 1MB 内容
    std::string large_content(1024 * 1024, 'A');
    for (size_t i = 0; i < large_content.size(); i++) {
        large_content[i] = static_cast<char>('A' + (i % 26));
    }

    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "large.bin";
        e.type = EntryType::REGULAR_FILE;
        e.size = large_content.size();
        std::istringstream content(large_content);
        writer->add_entry(e, content);
        writer->commit();
    }

    auto reader = open_archive(path);
    reader->validate();

    EntryInfo info;
    reader->next_entry(info);
    EXPECT_EQ(info.path, "large.bin");
    EXPECT_EQ(info.size, large_content.size());

    auto stream = reader->open_content(info);
    ASSERT_NE(stream, nullptr);
    std::ostringstream oss;
    oss << stream->rdbuf();
    EXPECT_EQ(oss.str().size(), large_content.size());
    EXPECT_EQ(oss.str().substr(0, 100), large_content.substr(0, 100));
    EXPECT_EQ(oss.str().substr(large_content.size() - 100), large_content.substr(large_content.size() - 100));
}

// ===== 4. 空文件内容 =====
TEST(ArchiveFunctional, EmptyFileContent) {
    TempDir tmp;
    auto path = tmp.path() + "/empty_file.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "empty.txt";
        e.type = EntryType::REGULAR_FILE;
        e.size = 0;
        std::istringstream content("");
        writer->add_entry(e, content);
        writer->commit();
    }

    auto reader = open_archive(path);
    reader->validate();

    EntryInfo info;
    reader->next_entry(info);
    auto stream = reader->open_content(info);
    ASSERT_NE(stream, nullptr);
    std::ostringstream oss;
    oss << stream->rdbuf();
    EXPECT_EQ(oss.str(), "");
}

// ===== 5. 混合条目类型 =====
TEST(ArchiveFunctional, MixedEntryTypes) {
    TempDir tmp;
    auto path = tmp.path() + "/mixed.dat";
    {
        auto writer = create_archive(path);

        EntryInfo dir;
        dir.path = "docs";
        dir.type = EntryType::DIRECTORY;
        writer->add_entry(dir);

        EntryInfo file;
        file.path = "docs/readme.txt";
        file.type = EntryType::REGULAR_FILE;
        file.size = 5;
        std::istringstream content("hello");
        writer->add_entry(file, content);

        EntryInfo link;
        link.path = "docs/link";
        link.type = EntryType::SYMBOLIC_LINK;
        link.link_target = "readme.txt";
        writer->add_entry(link);

        writer->commit();
    }

    auto reader = open_archive(path);
    reader->validate();

    int dir_count = 0, file_count = 0, link_count = 0;
    while (reader->has_next_entry()) {
        EntryInfo info;
        reader->next_entry(info);
        switch (info.type) {
            case EntryType::DIRECTORY: dir_count++; break;
            case EntryType::REGULAR_FILE: file_count++; break;
            case EntryType::SYMBOLIC_LINK: link_count++; break;
            default: break;
        }
    }
    EXPECT_EQ(dir_count, 1);
    EXPECT_EQ(file_count, 1);
    EXPECT_EQ(link_count, 1);
}

// ===== 6. Abort 后提交&读取失败 =====
TEST(ArchiveFunctional, AbortThenCommitFails) {
    TempDir tmp;
    auto path = tmp.path() + "/abort_then_commit.dat";
    auto writer = create_archive(path);

    EntryInfo e;
    e.path = "lost.txt";
    e.type = EntryType::REGULAR_FILE;
    std::istringstream content("data");
    writer->add_entry(e, content);

    writer->abort();

    // 再 commit 应该失败
    auto r = writer->commit();
    EXPECT_EQ(r.status, Status::FAILED);

    // 归档文件应不存在
    EXPECT_FALSE(std::filesystem::exists(path));
}

// ===== 7. 重复读取内容 =====
TEST(ArchiveFunctional, ReopenContent) {
    TempDir tmp;
    auto path = tmp.path() + "/reopen.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "data.txt";
        e.type = EntryType::REGULAR_FILE;
        e.size = 3;
        std::istringstream content("abc");
        writer->add_entry(e, content);
        writer->commit();
    }

    auto reader = open_archive(path);
    reader->validate();

    EntryInfo info;
    reader->next_entry(info);

    // 多次 open_content
    for (int i = 0; i < 3; i++) {
        auto stream = reader->open_content(info);
        ASSERT_NE(stream, nullptr);
        std::ostringstream oss;
        oss << stream->rdbuf();
        EXPECT_EQ(oss.str(), "abc");
    }
}

// ===== 8. 不 commit 直接析构 =====
TEST(ArchiveFunctional, DestructorWithoutCommit) {
    TempDir tmp;
    auto path = tmp.path() + "/no_commit.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "temp.txt";
        e.type = EntryType::REGULAR_FILE;
        std::istringstream content("data");
        writer->add_entry(e, content);
        // 析构时自动 abort
    }
    // 临时文件应被清理，最终文件不应存在
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
}

// ===== 9. 路径含特殊字符 =====
TEST(ArchiveFunctional, SpecialCharactersInPath) {
    TempDir tmp;
    auto path = tmp.path() + "/special.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "dir/hello world/文件.txt";
        e.type = EntryType::REGULAR_FILE;
        e.size = 4;
        std::istringstream content("test");
        writer->add_entry(e, content);
        writer->commit();
    }

    auto reader = open_archive(path);
    reader->validate();

    EntryInfo info;
    reader->next_entry(info);
    EXPECT_EQ(info.path, "dir/hello world/文件.txt");
}

// ===== 10. 先 abort 再创建同名归档 =====
TEST(ArchiveFunctional, AbortThenCreateSameName) {
    TempDir tmp;
    auto path = tmp.path() + "/retry.dat";

    // 第一次 abort
    auto w1 = create_archive(path);
    w1->abort();

    // 第二次成功
    auto w2 = create_archive(path);
    EntryInfo e;
    e.path = "success.txt";
    e.type = EntryType::REGULAR_FILE;
    std::istringstream content("ok");
    w2->add_entry(e, content);
    w2->commit();

    EXPECT_TRUE(std::filesystem::exists(path));

    auto reader = open_archive(path);
    EXPECT_EQ(reader->validate().status, Status::SUCCESS);
}

// ===== 11. 流式写入 — 大输入流分块 =====
TEST(ArchiveFunctional, StreamWriteLargeContent) {
    TempDir tmp;
    auto path = tmp.path() + "/stream_large.dat";
    {
        auto writer = create_archive(path);

        // 模拟大文件流：生成 2MB 内容
        std::string chunk(65536, 'A');
        std::ostringstream large_content;
        for (int i = 0; i < 32; i++) {
            large_content << chunk;
        }
        std::string expected = large_content.str();

        EntryInfo e;
        e.path = "large_stream.bin";
        e.type = EntryType::REGULAR_FILE;
        e.size = expected.size();
        std::istringstream content_stream(expected);
        writer->add_entry(e, content_stream);
        writer->commit();
    }

    auto reader = open_archive(path);
    reader->validate();
    EntryInfo info;
    reader->next_entry(info);
    EXPECT_EQ(info.path, "large_stream.bin");
    EXPECT_EQ(info.size, 2 * 1024 * 1024);

    auto stream = reader->open_content(info);
    ASSERT_NE(stream, nullptr);
    std::ostringstream oss;
    oss << stream->rdbuf();
    EXPECT_EQ(oss.str().size(), 2 * 1024 * 1024);
}

// ===== 12. 流式写入 — 多次 add_entry 交替 =====
TEST(ArchiveFunctional, StreamMultipleEntries) {
    TempDir tmp;
    auto path = tmp.path() + "/multi_stream.dat";
    const int N = 50;
    {
        auto writer = create_archive(path);
        for (int i = 0; i < N; i++) {
            EntryInfo e;
            e.path = "file_" + std::to_string(i) + ".dat";
            e.type = EntryType::REGULAR_FILE;
            e.size = 100;
            std::string data(100, 'A' + (i % 26));
            std::istringstream content(data);
            auto r = writer->add_entry(e, content);
            EXPECT_EQ(r.status, Status::SUCCESS);
        }
        writer->commit();
    }

    auto reader = open_archive(path);
    reader->validate();
    int count = 0;
    while (reader->has_next_entry()) {
        EntryInfo info;
        reader->next_entry(info);
        EXPECT_EQ(info.type, EntryType::REGULAR_FILE);
        auto stream = reader->open_content(info);
        ASSERT_NE(stream, nullptr);
        std::ostringstream oss;
        oss << stream->rdbuf();
        EXPECT_EQ(oss.str().size(), 100);
        count++;
    }
    EXPECT_EQ(count, N);
}

// ===== 13. 边界 — 1 字节文件 =====
TEST(ArchiveFunctional, SingleByteFile) {
    TempDir tmp;
    auto path = tmp.path() + "/1byte.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "x";
        e.type = EntryType::REGULAR_FILE;
        e.size = 1;
        std::istringstream content("A");
        writer->add_entry(e, content);
        writer->commit();
    }
    auto reader = open_archive(path);
    EXPECT_EQ(reader->validate().status, Status::SUCCESS);
    EntryInfo info;
    reader->next_entry(info);
    auto s = reader->open_content(info);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->get(), 'A');
}

// ===== 14. 边界 — 内容等于缓冲区大小（8192 字节）=====
TEST(ArchiveFunctional, ContentExactlyBufferSize) {
    TempDir tmp;
    auto path = tmp.path() + "/bufsize.dat";
    std::string content(8192, 'B');
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "buf.txt";
        e.type = EntryType::REGULAR_FILE;
        e.size = content.size();
        std::istringstream iss(content);
        writer->add_entry(e, iss);
        writer->commit();
    }
    auto reader = open_archive(path);
    reader->validate();
    EntryInfo info;
    reader->next_entry(info);
    auto s = reader->open_content(info);
    ASSERT_NE(s, nullptr);
    std::ostringstream oss;
    oss << s->rdbuf();
    EXPECT_EQ(oss.str().size(), 8192);
    EXPECT_EQ(oss.str(), content);
}

// ===== 15. 边界 — 内容跨缓冲区边界（8193 字节）=====
TEST(ArchiveFunctional, ContentCrossBufferBoundary) {
    TempDir tmp;
    auto path = tmp.path() + "/cross.dat";
    std::string content(8193, 'C');
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "cross.txt";
        e.type = EntryType::REGULAR_FILE;
        e.size = content.size();
        std::istringstream iss(content);
        writer->add_entry(e, iss);
        writer->commit();
    }
    auto reader = open_archive(path);
    reader->validate();
    EntryInfo info;
    reader->next_entry(info);
    auto s = reader->open_content(info);
    ASSERT_NE(s, nullptr);
    std::ostringstream oss;
    oss << s->rdbuf();
    EXPECT_EQ(oss.str().size(), 8193);
    EXPECT_EQ(oss.str(), content);
}

// ===== 16. 边界 — 7 种条目类型全部写入 =====
TEST(ArchiveFunctional, AllSevenEntryTypes) {
    TempDir tmp;
    auto path = tmp.path() + "/alltypes.dat";
    {
        auto writer = create_archive(path);
        auto add = [&](EntryType t, const std::string& p) {
            EntryInfo e;
            e.path = p;
            e.type = t;
            if (t == EntryType::REGULAR_FILE) {
                e.size = 4;
                std::istringstream c("data");
                writer->add_entry(e, c);
            } else if (t == EntryType::SYMBOLIC_LINK) {
                e.link_target = "target";
                writer->add_entry(e);
            } else {
                writer->add_entry(e);
            }
        };
        add(EntryType::REGULAR_FILE, "f.txt");
        add(EntryType::DIRECTORY, "dir");
        add(EntryType::SYMBOLIC_LINK, "link");
        add(EntryType::HARD_LINK, "hard");
        add(EntryType::FIFO, "pipe");
        add(EntryType::CHARACTER_DEVICE, "tty");
        add(EntryType::BLOCK_DEVICE, "sda");
        writer->commit();
    }
    auto reader = open_archive(path);
    EXPECT_EQ(reader->validate().status, Status::SUCCESS);
    int count = 0;
    while (reader->has_next_entry()) {
        EntryInfo info;
        EXPECT_EQ(reader->next_entry(info).status, Status::SUCCESS);
        count++;
    }
    EXPECT_EQ(count, 7);
}

// ===== 17. 边界 — commit 两次第二次失败 =====
TEST(ArchiveFunctional, DoubleCommitFails) {
    TempDir tmp;
    auto path = tmp.path() + "/double_commit.dat";
    auto writer = create_archive(path);
    EXPECT_EQ(writer->commit().status, Status::SUCCESS);
    auto r = writer->commit();
    EXPECT_EQ(r.status, Status::FAILED);
}
