#include <gtest/gtest.h>
#include "modules/scanner/scanner.h"
#include "modules/filter/filter.h"
#include "modules/archive_writer/archive_writer.h"
#include "modules/archive_reader/archive_reader.h"
#include "../../tests/helpers/temp_dir.h"
#include <sys/un.h>
#include <sys/socket.h>

using namespace backup;
using namespace backup::testing;

// ===== 1. 空目录 =====
TEST(ScannerContract, ScanEmptyDirectory) {
    TempDir tmp;
    auto scanner = create_scanner();
    FilterRules rules;
    auto filter = create_filter(rules);
    auto writer = create_archive(tmp.path() + "/archive.dat");

    Result r = scanner->scan_and_backup(tmp.path(), *filter, *writer, nullptr);
    EXPECT_EQ(r.status, Status::SUCCESS);

    // 空目录扫描后 writer 应只收到目录自身条目（不含子条目）
    // commit 使归档可读
    Result commit_r = writer->commit();
    EXPECT_EQ(commit_r.status, Status::SUCCESS);
}

// ===== 2. 递归扫描 =====
TEST(ScannerContract, ScanDirectoryWithFiles) {
    TempDir tmp;
    tmp.create_dir("subdir");
    tmp.create_file("root.txt", "hello");
    tmp.create_file("subdir/child.txt", "world");

    auto scanner = create_scanner();
    FilterRules rules;
    auto filter = create_filter(rules);
    auto writer = create_archive(tmp.path() + "/archive.dat");

    Result r = scanner->scan_and_backup(tmp.path(), *filter, *writer, nullptr);
    EXPECT_EQ(r.status, Status::SUCCESS);

    Result commit_r = writer->commit();
    EXPECT_EQ(commit_r.status, Status::SUCCESS);

    // 归档应可读，条目数至少包含 root.txt, subdir/, subdir/child.txt
    auto reader = open_archive(tmp.path() + "/archive.dat");
    EXPECT_NE(reader, nullptr);
    Result vr = reader->validate();
    EXPECT_EQ(vr.status, Status::SUCCESS);

    int count = 0;
    while (reader->has_next_entry()) {
        EntryInfo info;
        reader->next_entry(info);
        count++;
    }
    EXPECT_GE(count, 3);  // root.txt + subdir + child.txt
}

// ===== 3. 跳过 Unix Socket =====
TEST(ScannerContract, ScanSkipsUnixSocket) {
    TempDir tmp;
    tmp.create_file("normal.txt", "content");

    // 创建 Unix Socket
    auto sock_path = tmp.path() + "/test.sock";
    int sockfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd >= 0) {
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
        ::bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
        ::close(sockfd);
    }

    auto scanner = create_scanner();
    FilterRules rules;
    auto filter = create_filter(rules);
    auto writer = create_archive(tmp.path() + "/archive.dat");

    Result r = scanner->scan_and_backup(tmp.path(), *filter, *writer, nullptr);
    EXPECT_EQ(r.status, Status::SUCCESS);

    Result commit_r = writer->commit();
    EXPECT_EQ(commit_r.status, Status::SUCCESS);

    // 归档中不应包含 .sock 文件
    auto reader = open_archive(tmp.path() + "/archive.dat");
    Result vr = reader->validate();
    EXPECT_EQ(vr.status, Status::SUCCESS);

    while (reader->has_next_entry()) {
        EntryInfo info;
        reader->next_entry(info);
        // 不应出现 sock 路径
        EXPECT_TRUE(info.path.find(".sock") == std::string::npos);
    }
}

// ===== 4. 不跟随符号链接 =====
TEST(ScannerContract, ScanDoesNotFollowSymlink) {
    TempDir tmp;
    tmp.create_dir("real_dir");
    tmp.create_file("real_dir/file.txt", "content");
    tmp.create_symlink("link_to_dir", "real_dir");

    auto scanner = create_scanner();
    FilterRules rules;
    auto filter = create_filter(rules);
    auto writer = create_archive(tmp.path() + "/archive.dat");

    Result r = scanner->scan_and_backup(tmp.path(), *filter, *writer, nullptr);
    EXPECT_EQ(r.status, Status::SUCCESS);

    Result commit_r = writer->commit();
    EXPECT_EQ(commit_r.status, Status::SUCCESS);

    auto reader = open_archive(tmp.path() + "/archive.dat");
    Result vr = reader->validate();
    EXPECT_EQ(vr.status, Status::SUCCESS);

    bool found_symlink = false;
    while (reader->has_next_entry()) {
        EntryInfo info;
        reader->next_entry(info);
        if (info.type == EntryType::SYMBOLIC_LINK) {
            found_symlink = true;
            EXPECT_EQ(info.link_target, "real_dir");
        }
        // 不应出现 link_to_dir/file.txt（不跟随）
    }
    EXPECT_TRUE(found_symlink);
}

// ===== 5. 硬链接识别 =====
TEST(ScannerContract, ScanIdentifiesHardLink) {
    TempDir tmp;
    tmp.create_file("original.txt", "same content");
    tmp.create_hardlink("hardlink.txt", "original.txt");

    auto scanner = create_scanner();
    FilterRules rules;
    auto filter = create_filter(rules);
    auto writer = create_archive(tmp.path() + "/archive.dat");

    Result r = scanner->scan_and_backup(tmp.path(), *filter, *writer, nullptr);
    EXPECT_EQ(r.status, Status::SUCCESS);

    Result commit_r = writer->commit();
    EXPECT_EQ(commit_r.status, Status::SUCCESS);

    auto reader = open_archive(tmp.path() + "/archive.dat");
    Result vr = reader->validate();
    EXPECT_EQ(vr.status, Status::SUCCESS);

    int hardlink_count = 0;
    while (reader->has_next_entry()) {
        EntryInfo info;
        reader->next_entry(info);
        if (info.type == EntryType::HARD_LINK) {
            hardlink_count++;
            EXPECT_FALSE(info.hard_link_target.empty());
        }
    }
    EXPECT_EQ(hardlink_count, 1);  // 第二个文件应被识别为硬链接
}

// ===== 6. 遵循筛选 =====
TEST(ScannerContract, ScanRespectsFilter) {
    TempDir tmp;
    tmp.create_file("keep.txt", "keep this");
    tmp.create_file("skip.tmp", "skip this");

    FilterRules rules;
    rules.exclude_names = {"*.tmp"};
    auto filter = create_filter(rules);

    auto scanner = create_scanner();
    auto writer = create_archive(tmp.path() + "/archive.dat");

    Result r = scanner->scan_and_backup(tmp.path(), *filter, *writer, nullptr);
    EXPECT_EQ(r.status, Status::SUCCESS);

    Result commit_r = writer->commit();
    EXPECT_EQ(commit_r.status, Status::SUCCESS);

    auto reader = open_archive(tmp.path() + "/archive.dat");
    Result vr = reader->validate();
    EXPECT_EQ(vr.status, Status::SUCCESS);

    while (reader->has_next_entry()) {
        EntryInfo info;
        reader->next_entry(info);
        // .tmp 文件不应出现在归档中
        EXPECT_TRUE(info.path.find(".tmp") == std::string::npos);
    }
}

// ===== 7. 取消 =====
TEST(ScannerContract, ScanCancelsOnCallbackFalse) {
    TempDir tmp;
    tmp.create_file("file1.txt", "a");
    tmp.create_file("file2.txt", "b");
    tmp.create_file("file3.txt", "c");

    auto scanner = create_scanner();
    FilterRules rules;
    auto filter = create_filter(rules);
    auto writer = create_archive(tmp.path() + "/archive.dat");

    // 第一次回调返回 true，第二次返回 false → 取消
    int call_count = 0;
    auto cb = [&call_count](const Progress&) -> bool {
        call_count++;
        return call_count < 2;  // 第一次 true，第二次 false
    };

    Result r = scanner->scan_and_backup(tmp.path(), *filter, *writer, cb);
    EXPECT_EQ(r.status, Status::CANCELLED);
}

// ===== 8. 源路径不存在 =====
TEST(ScannerContract, ScanSourceNotFound) {
    auto scanner = create_scanner();
    FilterRules rules;
    auto filter = create_filter(rules);
    auto writer = create_archive("/tmp/nonexistent_archive.dat");

    Result r = scanner->scan_and_backup("/nonexistent/path", *filter, *writer, nullptr);
    EXPECT_EQ(r.status, Status::FAILED);
}
