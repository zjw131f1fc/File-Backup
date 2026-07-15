#pragma once

#include "common/entry_info.h"
#include <cstdint>
#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace backup::stub_archive {

inline constexpr uint64_t kMaxFieldSize = 64 * 1024 * 1024;
inline constexpr uint64_t kMaxContentSize = 1024 * 1024 * 1024;

inline constexpr char kHeader[] = "BACKUP_STUB_ARCHIVE_V2";
inline constexpr uint32_t kRecordMagic = 0x53545231;
inline constexpr uint32_t kEndMagic = 0x53544e44;

struct Record {
    EntryInfo entry;
    std::string content;
    bool has_content = false;
};

template <typename T>
bool write_value(std::ostream& output, T value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(output);
}

template <typename T>
bool read_value(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(input);
}

inline bool write_string(std::ostream& output, const std::string& value) {
    const auto size = static_cast<uint64_t>(value.size());
    return write_value(output, size) &&
        static_cast<bool>(output.write(value.data(), static_cast<std::streamsize>(value.size())));
}

inline bool read_string(std::istream& input, std::string& value) {
    uint64_t size = 0;
    if (!read_value(input, size) || size > kMaxFieldSize) {
        return false;
    }
    value.resize(static_cast<std::size_t>(size));
    input.read(value.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(input);
}

inline bool write_record(std::ostream& output, const Record& record) {
    const auto& entry = record.entry;
    const auto type = static_cast<uint8_t>(entry.type);
    const auto permissions = static_cast<uint64_t>(entry.permissions);
    const auto uid = static_cast<uint64_t>(entry.uid);
    const auto gid = static_cast<uint64_t>(entry.gid);
    const auto has_content = static_cast<uint8_t>(record.has_content ? 1 : 0);
    const auto content_size = static_cast<uint64_t>(record.content.size());

    return write_value(output, kRecordMagic) && write_string(output, entry.path) &&
        write_value(output, type) && write_value(output, entry.size) &&
        write_string(output, entry.link_target) && write_string(output, entry.hard_link_target) &&
        write_value(output, entry.hard_link_inode) && write_value(output, permissions) &&
        write_value(output, uid) && write_value(output, gid) &&
        write_value(output, entry.atime_sec) && write_value(output, entry.atime_nsec) &&
        write_value(output, entry.mtime_sec) && write_value(output, entry.mtime_nsec) &&
        write_value(output, entry.device_major) && write_value(output, entry.device_minor) &&
        write_value(output, has_content) && write_value(output, content_size) &&
        static_cast<bool>(output.write(record.content.data(),
                                       static_cast<std::streamsize>(record.content.size())));
}

inline bool read_record_body(std::istream& input, Record& record) {
    uint8_t type = 0;
    uint64_t permissions = 0;
    uint64_t uid = 0;
    uint64_t gid = 0;
    uint8_t has_content = 0;
    uint64_t content_size = 0;

    if (!read_string(input, record.entry.path) || !read_value(input, type) ||
        !read_value(input, record.entry.size) ||
        !read_string(input, record.entry.link_target) ||
        !read_string(input, record.entry.hard_link_target) ||
        !read_value(input, record.entry.hard_link_inode) ||
        !read_value(input, permissions) || !read_value(input, uid) ||
        !read_value(input, gid) || !read_value(input, record.entry.atime_sec) ||
        !read_value(input, record.entry.atime_nsec) ||
        !read_value(input, record.entry.mtime_sec) ||
        !read_value(input, record.entry.mtime_nsec) ||
        !read_value(input, record.entry.device_major) ||
        !read_value(input, record.entry.device_minor) ||
        !read_value(input, has_content) || !read_value(input, content_size)) {
        return false;
    }
    if (type > static_cast<uint8_t>(EntryType::BLOCK_DEVICE) ||
        has_content > 1 || content_size > kMaxContentSize) {
        return false;
    }

    record.entry.type = static_cast<EntryType>(type);
    record.entry.permissions = static_cast<mode_t>(permissions);
    record.entry.uid = static_cast<uid_t>(uid);
    record.entry.gid = static_cast<gid_t>(gid);
    record.has_content = has_content != 0;
    record.content.resize(static_cast<std::size_t>(content_size));
    input.read(record.content.data(), static_cast<std::streamsize>(content_size));
    return static_cast<bool>(input);
}

inline bool read_record(std::istream& input, Record& record) {
    uint32_t magic = 0;
    return read_value(input, magic) && magic == kRecordMagic &&
        read_record_body(input, record);
}

inline bool write_archive(std::ostream& output, const std::vector<Record>& records) {
    output.write(kHeader, sizeof(kHeader) - 1);
    output.put('\n');
    for (const auto& record : records) {
        if (!write_record(output, record)) {
            return false;
        }
    }
    return write_value(output, kEndMagic);
}

inline bool is_safe_path(const std::string& path) {
    const std::filesystem::path parsed(path);
    if (parsed.empty() || parsed.is_absolute()) {
        return false;
    }
    for (const auto& part : parsed) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

}  // namespace backup::stub_archive
