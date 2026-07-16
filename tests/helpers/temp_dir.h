#pragma once

#include <string>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <unistd.h>

namespace backup::testing {

// 临时目录辅助类：构造时创建，析构时删除
class TempDir {
public:
    explicit TempDir(const std::string& prefix = "backup_test_") {
        auto base = std::filesystem::temp_directory_path();
        // 用 PID + 随机数保证唯一
        path_ = base / (prefix + std::to_string(::getpid()) + "_" +
                        std::to_string(rand_counter_++));
        std::filesystem::create_directories(path_);
        path_str_ = path_.string();
    }

    ~TempDir() {
        std::filesystem::remove_all(path_);
    }

    const std::string& path() const { return path_str_; }

    // 在临时目录下创建子目录
    std::string create_dir(const std::string& rel_path) {
        auto full = path_ / rel_path;
        std::filesystem::create_directories(full);
        return full.string();
    }

    // 在临时目录下创建普通文件并写入内容
    std::string create_file(const std::string& rel_path, const std::string& content = "") {
        auto full = path_ / rel_path;
        // 先确保父目录存在
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
        out.close();
        return full.string();
    }

    // 在临时目录下创建符号链接
    std::string create_symlink(const std::string& rel_path, const std::string& target) {
        auto full = path_ / rel_path;
        std::filesystem::create_directories(full.parent_path());
        // target 可以是相对路径或绝对路径
        std::filesystem::create_symlink(target, full);
        return full.string();
    }

    // 在临时目录下创建硬链接
    std::string create_hardlink(const std::string& rel_path, const std::string& existing_rel) {
        auto full = path_ / rel_path;
        auto existing = path_ / existing_rel;
        std::filesystem::create_directories(full.parent_path());
        std::filesystem::create_hard_link(existing, full);
        return full.string();
    }

    // 在临时目录下创建 FIFO
    std::string create_fifo(const std::string& rel_path) {
        auto full = path_ / rel_path;
        std::filesystem::create_directories(full.parent_path());
        mkfifo(full.string().c_str(), 0644);
        return full.string();
    }

private:
    std::filesystem::path path_;
    std::string path_str_ = path_.string();
    static int rand_counter_;
};

inline int TempDir::rand_counter_ = 0;

}  // namespace backup::testing
