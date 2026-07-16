#include "modules/archive_reader/archive_reader.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdio>

namespace backup {

namespace {

// 与 writer 保持一致
constexpr char MAGIC[4] = {'B', 'A', 'K', '1'};
constexpr uint32_t FORMAT_VERSION = 1;

// 小端读取工具
template<typename T>
T read_le(std::istream& is) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        unsigned char c;
        is.read(reinterpret_cast<char*>(&c), 1);
        if (!is) return 0;
        value |= static_cast<T>(c) << (i * 8);
    }
    return value;
}

std::string read_padded_string(std::istream& is) {
    uint16_t len = read_le<uint16_t>(is);
    if (len == 0 || !is) return {};
    std::string str(len, '\0');
    is.read(&str[0], len);
    return str;
}

// 检查路径是否危险（绝对路径或包含 .. 穿越）
bool is_dangerous_path(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/') return true;
    // 检查是否存在 .. 路径穿越
    std::string::size_type pos = 0;
    while (pos < path.size()) {
        auto slash = path.find('/', pos);
        std::string component;
        if (slash == std::string::npos) {
            component = path.substr(pos);
            pos = path.size();
        } else {
            component = path.substr(pos, slash - pos);
            pos = slash + 1;
        }
        if (component == "..") return true;
    }
    return false;
}

// 缓存的条目信息
struct CachedEntry {
    EntryInfo info;
    std::streamoff content_offset;  // 文件内容在归档中的偏移（-1 表示无内容）
    uint64_t content_len;
};

// ===== 流式读取 streambuf =====
// 从文件的指定偏移开始，最多读取 limit 字节，不一次性加载到内存
class LimitedStreamBuf : public std::streambuf {
public:
    LimitedStreamBuf(const std::string& path, std::streamoff offset, uint64_t limit)
        : file_(path, std::ios::binary), limit_(limit), pos_(0) {
        if (!file_.seekg(offset)) {
            file_.setstate(std::ios::badbit);
        }
    }

    bool is_open() const { return file_.is_open() && file_.good(); }

protected:
    int underflow() override {
        if (pos_ >= limit_ || file_.eof()) {
            return traits_type::eof();
        }
        uint64_t to_read = std::min<uint64_t>(sizeof(buf_), limit_ - pos_);
        file_.read(buf_, to_read);
        std::streamsize got = file_.gcount();
        if (got <= 0) {
            return traits_type::eof();
        }
        pos_ += got;
        setg(buf_, buf_, buf_ + got);
        return traits_type::to_int_type(*gptr());
    }

private:
    std::ifstream file_;
    uint64_t limit_;
    uint64_t pos_;
    char buf_[8192];
};

}  // anonymous namespace

class ArchiveReader : public IArchiveReader {
public:
    explicit ArchiveReader(const std::string& archive_path)
        : archive_path_(archive_path)
        , validated_(false)
        , current_index_(0) {}

    Result validate() override {
        if (validated_) {
            return success("already validated");
        }

        std::ifstream ifs(archive_path_, std::ios::binary);
        if (!ifs) {
            return failed("cannot open archive: " + archive_path_);
        }

        // 检查 magic
        char magic[4];
        ifs.read(magic, 4);
        if (!ifs || std::memcmp(magic, MAGIC, 4) != 0) {
            return failed("invalid archive format (bad magic)");
        }

        // 检查版本
        uint32_t version = read_le<uint32_t>(ifs);
        if (version != FORMAT_VERSION) {
            return failed("unsupported archive version");
        }

        // 扫描所有条目
        std::vector<CachedEntry> entries;
        bool has_dangerous = false;
        bool truncated = false;
        auto file_size = std::filesystem::file_size(archive_path_);

        while (static_cast<uint64_t>(ifs.tellg()) < file_size) {
            // path
            auto path = read_padded_string(ifs);
            if (!ifs || path.empty()) {
                truncated = true;
                break;
            }

            // 检查危险路径
            if (is_dangerous_path(path)) {
                has_dangerous = true;
            }

            // type
            auto type_val = read_le<uint8_t>(ifs);

            // size
            auto size = read_le<uint64_t>(ifs);

            // mode
            auto mode = read_le<uint32_t>(ifs);
            auto uid = read_le<uint32_t>(ifs);
            auto gid = read_le<uint32_t>(ifs);

            // atime
            auto atime_sec = read_le<int64_t>(ifs);
            auto atime_nsec = read_le<int32_t>(ifs);
            auto mtime_sec = read_le<int64_t>(ifs);
            auto mtime_nsec = read_le<int32_t>(ifs);

            // link_target
            auto link_target = read_padded_string(ifs);
            auto hard_link_target = read_padded_string(ifs);

            // device numbers
            auto dev_major = read_le<uint32_t>(ifs);
            auto dev_minor = read_le<uint32_t>(ifs);

            // content_len
            auto content_len = read_le<uint64_t>(ifs);

            if (!ifs) {
                truncated = true;
                break;
            }

            CachedEntry ce;
            ce.info.path = path;
            ce.info.type = static_cast<EntryType>(type_val);
            ce.info.size = size;
            ce.info.permissions = static_cast<mode_t>(mode);
            ce.info.uid = static_cast<uid_t>(uid);
            ce.info.gid = static_cast<gid_t>(gid);
            ce.info.atime_sec = atime_sec;
            ce.info.atime_nsec = atime_nsec;
            ce.info.mtime_sec = mtime_sec;
            ce.info.mtime_nsec = mtime_nsec;
            ce.info.link_target = link_target;
            ce.info.hard_link_target = hard_link_target;
            ce.info.device_major = dev_major;
            ce.info.device_minor = dev_minor;
            ce.content_offset = ifs.tellg();
            ce.content_len = content_len;

            entries.push_back(std::move(ce));

            // 跳过 content 到下一个条目
            if (content_len > 0) {
                ifs.seekg(content_len, std::ios::cur);
                if (!ifs) {
                    truncated = true;
                    break;
                }
            }
        }

        // 检查是否还有未读的垃圾数据
        if (!truncated && static_cast<uint64_t>(ifs.tellg()) != file_size) {
            truncated = true;
        }

        entries_ = std::move(entries);
        validated_ = true;

        if (truncated) {
            return failed("archive is truncated");
        }

        if (has_dangerous) {
            return failed("archive contains dangerous paths");
        }

        return success("archive validated");
    }

    bool has_next_entry() override {
        return validated_ && current_index_ < entries_.size();
    }

    Result next_entry(EntryInfo& entry_info) override {
        if (!validated_) {
            return failed("archive not validated");
        }
        if (current_index_ >= entries_.size()) {
            return failed("no more entries");
        }

        entry_info = entries_[current_index_].info;
        current_index_++;
        return success("");
    }

    // 自定义 istream：析构时自动删除关联的 streambuf
    class OwningStream : public std::istream {
    public:
        explicit OwningStream(std::streambuf* sb) : std::istream(sb), sb_(sb) {}
        ~OwningStream() override { delete sb_; }
    private:
        std::streambuf* sb_;
    };

    std::unique_ptr<std::istream> open_content(const EntryInfo& entry_info) override {
        for (const auto& ce : entries_) {
            if (ce.info.path == entry_info.path) {
                if (ce.info.type != EntryType::REGULAR_FILE) {
                    return nullptr;
                }
                if (ce.content_len == 0) {
                    return std::make_unique<std::istringstream>();
                }
                // 流式读取：从文件直接流式读取，不加载全部内容到内存
                auto buf = new LimitedStreamBuf(
                    archive_path_, ce.content_offset, ce.content_len);
                if (!buf->is_open()) {
                    delete buf;
                    return nullptr;
                }
                return std::make_unique<OwningStream>(buf);
            }
        }
        return nullptr;
    }

private:
    std::string archive_path_;
    bool validated_;
    std::vector<CachedEntry> entries_;
    size_t current_index_;

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

std::unique_ptr<IArchiveReader> open_archive(const std::string& archive_path) {
    return std::make_unique<ArchiveReader>(archive_path);
}

}  // namespace backup
