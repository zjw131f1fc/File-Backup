#include "modules/archive_writer/archive_writer.h"
#include <fstream>
#include <filesystem>
#include <vector>
#include <optional>

namespace backup {

namespace {

// 自定义归档格式常量
constexpr char MAGIC[4] = {'B', 'A', 'K', '1'};
constexpr uint32_t FORMAT_VERSION = 1;

// 小端写入工具函数
template<typename T>
void write_le(std::ostream& os, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        os.put(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

void write_padded_string(std::ostream& os, const std::string& str) {
    uint16_t len = static_cast<uint16_t>(str.size());
    write_le(os, len);
    if (len > 0) os.write(str.data(), len);
}

// 写入条目头部（不含 content_len，由调用方处理）
void write_entry_header_to(std::ostream& os, const EntryInfo& entry) {
    write_padded_string(os, entry.path);
    write_le(os, static_cast<uint8_t>(entry.type));
    write_le(os, entry.size);
    write_le(os, static_cast<uint32_t>(entry.permissions));
    write_le(os, static_cast<uint32_t>(entry.uid));
    write_le(os, static_cast<uint32_t>(entry.gid));
    write_le(os, entry.atime_sec);
    write_le(os, static_cast<int32_t>(entry.atime_nsec));
    write_le(os, entry.mtime_sec);
    write_le(os, static_cast<int32_t>(entry.mtime_nsec));
    write_padded_string(os, entry.link_target);
    write_padded_string(os, entry.hard_link_target);
    write_le(os, entry.device_major);
    write_le(os, entry.device_minor);
}

}  // anonymous namespace

class ArchiveWriter : public IArchiveWriter {
public:
    explicit ArchiveWriter(const std::string& output_path)
        : output_path_(output_path)
        , temp_path_(output_path + ".tmp")
        , state_(State::ACTIVE) {
        ofs_.open(temp_path_, std::ios::binary);
        if (!ofs_) {
            state_ = State::FAILED;
            return;
        }
        ofs_.write(MAGIC, 4);
        write_le(ofs_, FORMAT_VERSION);
    }

    ~ArchiveWriter() {
        close_stream();
        if (state_ != State::COMMITTED && std::filesystem::exists(temp_path_)) {
            std::filesystem::remove(temp_path_);
        }
    }

    Result add_entry(const EntryInfo& entry_info, std::istream& content) override {
        if (state_ != State::ACTIVE) {
            return failed("writer is no longer active");
        }

        // 1. 写入条目头部（不含 content_len）
        write_entry_header_to(ofs_, entry_info);

        // 2. 占位 content_len（8字节），记住位置
        auto content_len_pos = ofs_.tellp();
        write_le(ofs_, static_cast<uint64_t>(0));

        // 3. 流式写入文件内容并计数
        std::vector<char> buf(8192);
        uint64_t actual_size = 0;
        while (content) {
            content.read(buf.data(), buf.size());
            std::streamsize n = content.gcount();
            if (content.bad()) {
                // 流发生不可恢复错误
                return failed("input stream read error for " + entry_info.path);
            }
            if (n > 0) {
                ofs_.write(buf.data(), n);
                if (!ofs_) {
                    return failed("failed to write content for " + entry_info.path);
                }
                actual_size += n;
            }
        }

        // 4. 回填 content_len
        ofs_.seekp(content_len_pos);
        write_le(ofs_, actual_size);
        ofs_.seekp(0, std::ios::end);

        // 检查实际大小与声明是否一致（若 entry_info.size > 0）
        if (entry_info.size > 0 && actual_size != entry_info.size) {
            return failed("content size mismatch for " + entry_info.path
                          + ": expected " + std::to_string(entry_info.size)
                          + ", got " + std::to_string(actual_size));
        }

        return success("added entry " + entry_info.path);
    }

    Result add_entry(const EntryInfo& entry_info) override {
        if (state_ != State::ACTIVE) {
            return failed("writer is no longer active");
        }

        write_entry_header_to(ofs_, entry_info);
        // 非文件条目 content_len = 0
        write_le(ofs_, static_cast<uint64_t>(0));
        return success("added entry " + entry_info.path);
    }

    Result commit() override {
        if (state_ != State::ACTIVE) {
            return failed("writer is no longer active");
        }

        ofs_.close();
        state_ = State::COMMITTED;

        std::error_code ec;
        std::filesystem::rename(temp_path_, output_path_, ec);
        if (ec) {
            std::filesystem::remove(temp_path_);
            return failed("rename temp file failed: " + ec.message());
        }

        return success("committed");
    }

    Result abort() override {
        if (state_ != State::ACTIVE) {
            return failed("writer is no longer active");
        }

        close_stream();
        state_ = State::ABORTED;

        // 只清理临时文件，绝不碰 output_path（可能已存在的用户文件）
        if (std::filesystem::exists(temp_path_)) {
            std::filesystem::remove(temp_path_);
        }

        return success("aborted");
    }

private:
    enum class State { ACTIVE, COMMITTED, ABORTED, FAILED };

    std::string output_path_;
    std::string temp_path_;
    std::ofstream ofs_;
    State state_;

    void close_stream() {
        if (ofs_.is_open()) ofs_.close();
    }

    Result success(const std::string& msg) {
        Result r;
        r.status = Status::SUCCESS;
        r.message = msg;
        return r;
    }

    Result failed(const std::string& msg) {
        Result r;
        r.status = Status::FAILED;
        r.message = msg;
        return r;
    }
};

std::unique_ptr<IArchiveWriter> create_archive(const std::string& output_path) {
    return std::make_unique<ArchiveWriter>(output_path);
}

}  // namespace backup
