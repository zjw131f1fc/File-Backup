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
