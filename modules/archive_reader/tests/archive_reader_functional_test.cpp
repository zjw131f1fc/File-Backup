#include <gtest/gtest.h>
#include "modules/archive_reader/archive_reader.h"
#include "modules/archive_writer/archive_writer.h"
#include "../../tests/helpers/temp_dir.h"
#include <sstream>
#include <filesystem>

using namespace backup;
using namespace backup::testing;

// ============================================================
// 归档读取 — 功能测试
// ============================================================

// ===== 1. 不存在的文件 =====
TEST(ArchiveReaderFunctional, OpenNonExistentFile) {
    auto reader = open_archive("/nonexistent/path/archive.dat");
    EXPECT_NE(reader, nullptr);
    auto r = reader->validate();
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 2. 空文件 =====
TEST(ArchiveReaderFunctional, EmptyFileIsInvalid) {
    TempDir tmp;
    auto path = tmp.create_file("empty.dat", "");
    auto reader = open_archive(path);
    auto r = reader->validate();
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 3. 文件只有部分 magic =====
TEST(ArchiveReaderFunctional, PartialMagic) {
    TempDir tmp;
    auto path = tmp.create_file("partial.dat", "BAK");
    auto reader = open_archive(path);
    auto r = reader->validate();
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 4. 正确的 magic 但版本不对 =====
TEST(ArchiveReaderFunctional, WrongVersion) {
    TempDir tmp;
    auto path = tmp.path() + "/badver.dat";
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs.write("BAK1", 4);
        uint32_t version = 999;
        ofs.write(reinterpret_cast<const char*>(&version), 4);
    }
    auto reader = open_archive(path);
    auto r = reader->validate();
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 5. 归档末尾有额外垃圾数据 =====
TEST(ArchiveReaderFunctional, TrailingGarbage) {
    TempDir tmp;
    auto path = tmp.path() + "/trailing.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "file.txt";
        e.type = EntryType::REGULAR_FILE;
        std::istringstream content("data");
        writer->add_entry(e, content);
        writer->commit();
    }
    // 追加垃圾数据
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::app);
        ofs.write("GARBAGE", 7);
    }
    auto reader = open_archive(path);
    auto r = reader->validate();
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 6. 不 validate 直接 next_entry =====
TEST(ArchiveReaderFunctional, NextEntryWithoutValidate) {
    TempDir tmp;
    auto path = tmp.path() + "/novalidate.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "test.txt";
        e.type = EntryType::REGULAR_FILE;
        std::istringstream content("data");
        writer->add_entry(e, content);
        writer->commit();
    }

    auto reader = open_archive(path);
    // 没有调用 validate
    EXPECT_FALSE(reader->has_next_entry());

    EntryInfo info;
    auto r = reader->next_entry(info);
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 7. 多次 validate =====
TEST(ArchiveReaderFunctional, DoubleValidate) {
    TempDir tmp;
    auto path = tmp.path() + "/double.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "a.txt";
        e.type = EntryType::REGULAR_FILE;
        std::istringstream content("aaa");
        writer->add_entry(e, content);
        writer->commit();
    }

    auto reader = open_archive(path);
    EXPECT_EQ(reader->validate().status, Status::SUCCESS);
    // 第二次 validate 应仍然成功
    EXPECT_EQ(reader->validate().status, Status::SUCCESS);
    // 条目仍然可枚举
    EXPECT_TRUE(reader->has_next_entry());
}

// ===== 8. 遍历后 has_next_entry =====
TEST(ArchiveReaderFunctional, HasNextAfterFullEnumeration) {
    TempDir tmp;
    auto path = tmp.path() + "/fulldone.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e1;
        e1.path = "a.txt";
        e1.type = EntryType::REGULAR_FILE;
        std::istringstream c1("aaa");
        writer->add_entry(e1, c1);

        EntryInfo e2;
        e2.path = "b.txt";
        e2.type = EntryType::REGULAR_FILE;
        std::istringstream c2("bbb");
        writer->add_entry(e2, c2);
        writer->commit();
    }

    auto reader = open_archive(path);
    reader->validate();

    EXPECT_TRUE(reader->has_next_entry());
    EntryInfo i1;
    reader->next_entry(i1);
    EXPECT_TRUE(reader->has_next_entry());
    EntryInfo i2;
    reader->next_entry(i2);
    EXPECT_FALSE(reader->has_next_entry());
}

// ===== 9. 仅含目录的归档 =====
TEST(ArchiveReaderFunctional, DirectoriesOnly) {
    TempDir tmp;
    auto path = tmp.path() + "/dirs_only.dat";
    {
        auto writer = create_archive(path);
        for (auto& d : {"a", "a/b", "a/b/c", "x"}) {
            EntryInfo e;
            e.path = d;
            e.type = EntryType::DIRECTORY;
            writer->add_entry(e);
        }
        writer->commit();
    }

    auto reader = open_archive(path);
    reader->validate();

    int count = 0;
    while (reader->has_next_entry()) {
        EntryInfo info;
        reader->next_entry(info);
        EXPECT_EQ(info.type, EntryType::DIRECTORY);
        count++;
    }
    EXPECT_EQ(count, 4);
}

// ===== 10. 同时含绝对路径和穿越路径 =====
TEST(ArchiveReaderFunctional, MixedDangerousPaths) {
    TempDir tmp;
    auto path = tmp.path() + "/mixed_danger.dat";
    {
        auto writer = create_archive(path);
        EntryInfo safe;
        safe.path = "safe.txt";
        safe.type = EntryType::REGULAR_FILE;
        std::istringstream content("ok");
        writer->add_entry(safe, content);

        EntryInfo abs;
        abs.path = "/etc/passwd";
        abs.type = EntryType::REGULAR_FILE;
        writer->add_entry(abs);
        writer->commit();
    }

    auto reader = open_archive(path);
    auto r = reader->validate();
    EXPECT_EQ(r.status, Status::FAILED);
}

// ===== 11. 流式读取 — 分块读取内容 =====
TEST(ArchiveReaderFunctional, StreamReadInChunks) {
    TempDir tmp;
    auto path = tmp.path() + "/chunks.dat";
    std::string content = "0123456789ABCDEFGHIJ";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "data.bin";
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
    auto stream = reader->open_content(info);
    ASSERT_NE(stream, nullptr);

    char buf[5];
    std::string result;
    while (stream->read(buf, 5) || stream->gcount() > 0) {
        result.append(buf, stream->gcount());
    }
    EXPECT_EQ(result, content);
}

// ===== 12. 流式读取 — 逐字节读取 =====
TEST(ArchiveReaderFunctional, StreamReadByteByByte) {
    TempDir tmp;
    auto path = tmp.path() + "/bytes.dat";
    std::string content = "Hello!";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "f.txt";
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
    auto stream = reader->open_content(info);
    ASSERT_NE(stream, nullptr);

    std::string result;
    int c;
    while ((c = stream->get()) != EOF) {
        result.push_back(static_cast<char>(c));
    }
    EXPECT_EQ(result, content);
}

// ===== 13. 流式读取 — rdbuf 读取 =====
TEST(ArchiveReaderFunctional, StreamReadViaRdbuf) {
    TempDir tmp;
    auto path = tmp.path() + "/rdbuf.dat";
    std::string content(10000, 'X');
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "big.txt";
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
    auto stream = reader->open_content(info);
    ASSERT_NE(stream, nullptr);

    std::ostringstream oss;
    oss << stream->rdbuf();
    EXPECT_EQ(oss.str().size(), content.size());
    EXPECT_EQ(oss.str(), content);
}

// ===== 14. 流式读取 — 1MB 大文件分块读取 =====
TEST(ArchiveReaderFunctional, StreamLargeFile) {
    TempDir tmp;
    auto path = tmp.path() + "/large.dat";
    std::string content(1024 * 1024, 'A');
    for (size_t i = 0; i < content.size(); i++) {
        content[i] = static_cast<char>('A' + (i % 26));
    }
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "large.bin";
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
    auto stream = reader->open_content(info);
    ASSERT_NE(stream, nullptr);

    std::vector<char> buf(65536);
    uint64_t total = 0;
    while (stream->read(buf.data(), buf.size()) || stream->gcount() > 0) {
        total += stream->gcount();
    }
    EXPECT_EQ(total, content.size());
}

// ===== 15. 流式读取 — 多次 open_content =====
TEST(ArchiveReaderFunctional, StreamOpenMultipleTimes) {
    TempDir tmp;
    auto path = tmp.path() + "/multi.dat";
    std::string content = "stream data";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "f.txt";
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

    for (int i = 0; i < 3; i++) {
        auto stream = reader->open_content(info);
        ASSERT_NE(stream, nullptr);
        std::ostringstream oss;
        oss << stream->rdbuf();
        EXPECT_EQ(oss.str(), content);
    }
}

// ===== 16. 边界 — 内容恰好 8192 字节 =====
TEST(ArchiveReaderFunctional, ContentAtBufferBoundary) {
    TempDir tmp;
    auto path = tmp.path() + "/bnd.dat";
    std::string content(8192, 'X');
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "bnd.txt";
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
    char buf[8192];
    s->read(buf, 8192);
    EXPECT_EQ(s->gcount(), 8192);
    EXPECT_EQ(s->get(), EOF);  // 不能再多读
}

// ===== 17. 边界 — 内容跨缓冲区（8193 字节）=====
TEST(ArchiveReaderFunctional, ContentCrossBoundary) {
    TempDir tmp;
    auto path = tmp.path() + "/cross2.dat";
    std::string content(8193, 'Y');
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "cross2.txt";
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
    char buf[8193];
    s->read(buf, 8193);
    EXPECT_EQ(s->gcount(), 8193);
}

// ===== 18. 边界 — 不存在的条目 open_content =====
TEST(ArchiveReaderFunctional, OpenContentForNonexistentEntry) {
    TempDir tmp;
    auto path = tmp.path() + "/nonexist.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "real.txt";
        e.type = EntryType::REGULAR_FILE;
        e.size = 3;
        std::istringstream iss("abc");
        writer->add_entry(e, iss);
        writer->commit();
    }
    auto reader = open_archive(path);
    reader->validate();
    EntryInfo fake;
    fake.path = "not_exist.txt";
    EXPECT_EQ(reader->open_content(fake), nullptr);
}

// ===== 19. 边界 — 只读部分条目不读完 =====
TEST(ArchiveReaderFunctional, PartialEnumeration) {
    TempDir tmp;
    auto path = tmp.path() + "/partial.dat";
    {
        auto writer = create_archive(path);
        for (int i = 0; i < 5; i++) {
            EntryInfo e;
            e.path = "f" + std::to_string(i) + ".txt";
            e.type = EntryType::REGULAR_FILE;
            e.size = 1;
            std::istringstream iss("a");
            writer->add_entry(e, iss);
        }
        writer->commit();
    }
    auto reader = open_archive(path);
    reader->validate();
    EntryInfo info;
    reader->next_entry(info);
    reader->next_entry(info);
    EXPECT_TRUE(reader->has_next_entry());  // 还有3个
}

// ===== 20. 边界 — 路径包含空格和特殊字符 =====
TEST(ArchiveReaderFunctional, PathWithSpecialChars) {
    TempDir tmp;
    auto path = tmp.path() + "/special_path.dat";
    {
        auto writer = create_archive(path);
        EntryInfo e;
        e.path = "dir/sub dir/file (1).txt";
        e.type = EntryType::REGULAR_FILE;
        e.size = 4;
        std::istringstream iss("test");
        writer->add_entry(e, iss);
        writer->commit();
    }
    auto reader = open_archive(path);
    EXPECT_EQ(reader->validate().status, Status::SUCCESS);
    EntryInfo info;
    reader->next_entry(info);
    EXPECT_EQ(info.path, "dir/sub dir/file (1).txt");
}
