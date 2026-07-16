#include "modules/scanner/scanner.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace backup {

namespace {

Result failed_result(const std::string& message) {
    Result result;
    result.status = Status::FAILED;
    result.message = message;
    return result;
}

std::optional<EntryType> entry_type(mode_t mode) {
    if (S_ISREG(mode)) return EntryType::REGULAR_FILE;
    if (S_ISDIR(mode)) return EntryType::DIRECTORY;
    if (S_ISLNK(mode)) return EntryType::SYMBOLIC_LINK;
    if (S_ISFIFO(mode)) return EntryType::FIFO;
    if (S_ISCHR(mode)) return EntryType::CHARACTER_DEVICE;
    if (S_ISBLK(mode)) return EntryType::BLOCK_DEVICE;
    return std::nullopt;
}

bool fill_entry(const std::filesystem::path& path,
                const std::filesystem::path& source_root,
                EntryInfo& entry,
                const struct stat& metadata) {
    const auto type = entry_type(metadata.st_mode);
    if (!type) {
        return false;
    }

    entry = EntryInfo{};
    entry.path = path.lexically_relative(source_root).generic_string();
    entry.type = *type;
    entry.size = static_cast<uint64_t>(metadata.st_size);
    entry.permissions = metadata.st_mode & 07777;
    entry.uid = metadata.st_uid;
    entry.gid = metadata.st_gid;
    entry.atime_sec = metadata.st_atim.tv_sec;
    entry.atime_nsec = metadata.st_atim.tv_nsec;
    entry.mtime_sec = metadata.st_mtim.tv_sec;
    entry.mtime_nsec = metadata.st_mtim.tv_nsec;

    if (entry.type == EntryType::SYMBOLIC_LINK) {
        std::error_code error;
        entry.link_target = std::filesystem::read_symlink(path, error).generic_string();
        return !error;
    }
    if (entry.type == EntryType::CHARACTER_DEVICE ||
        entry.type == EntryType::BLOCK_DEVICE) {
        entry.device_major = major(metadata.st_rdev);
        entry.device_minor = minor(metadata.st_rdev);
    }
    return true;
}

}  // namespace

class Scanner : public IScanner {
public:
    Result scan_and_backup(const std::string& source_path,
                           IFilter& filter,
                           IArchiveWriter& archive_writer,
                           ProgressCallback progress_callback) override {
        std::error_code error;
        const auto source = std::filesystem::path(source_path);
        const auto source_status = std::filesystem::symlink_status(source, error);
        if (error || source_status.type() == std::filesystem::file_type::not_found) {
            return failed_result("source path does not exist: " + source_path);
        }
        if (!std::filesystem::is_directory(source_status)) {
            return failed_result("source path is not a directory: " + source_path);
        }

        std::map<std::pair<dev_t, ino_t>, std::string> hard_links;
        uint64_t processed_entries = 0;
        uint64_t processed_bytes = 0;
        std::filesystem::recursive_directory_iterator entries(
            source, std::filesystem::directory_options::none, error);
        if (error) {
            return failed_result("failed to scan source path: " + source_path);
        }

        for (auto it = entries; it != std::filesystem::recursive_directory_iterator();
             it.increment(error)) {
            if (error) {
                return failed_result("failed to scan source path: " + error.message());
            }

            struct stat metadata {};
            EntryInfo entry;
            if (::lstat(it->path().c_str(), &metadata) != 0) {
                return failed_result("failed to read entry: " + it->path().string());
            }
            if (S_ISSOCK(metadata.st_mode)) {
                continue;
            }
            if (!fill_entry(it->path(), source, entry, metadata)) {
                return failed_result("unsupported entry type: " + it->path().string());
            }
            if (!filter.should_include(entry)) {
                continue;
            }

            // Only archived paths may become hard-link targets. Registering an
            // excluded first occurrence would create a dangling archive entry.
            if (entry.type == EntryType::REGULAR_FILE && metadata.st_nlink > 1) {
                const auto key = std::make_pair(metadata.st_dev, metadata.st_ino);
                const auto existing = hard_links.find(key);
                if (existing != hard_links.end()) {
                    entry.type = EntryType::HARD_LINK;
                    entry.hard_link_target = existing->second;
                    entry.hard_link_inode = metadata.st_ino;
                } else {
                    hard_links.emplace(key, entry.path);
                }
            }

            Result write_result;
            if (entry.type == EntryType::REGULAR_FILE) {
                std::ifstream content(it->path(), std::ios::binary);
                if (!content) {
                    return failed_result("failed to open entry: " + it->path().string());
                }
                write_result = archive_writer.add_entry(entry, content);
            } else {
                write_result = archive_writer.add_entry(entry);
            }
            if (!write_result.ok()) {
                return write_result;
            }

            // Progress describes entries successfully handed to the archive,
            // rather than every filesystem object observed during traversal.
            ++processed_entries;
            if (entry.type == EntryType::REGULAR_FILE) {
                processed_bytes += entry.size;
            }
            Progress progress;
            progress.stage = "scanning";
            progress.processed_entries = processed_entries;
            progress.processed_bytes = processed_bytes;
            progress.current_path = entry.path;
            if (progress_callback && !progress_callback(progress)) {
                Result result;
                result.status = Status::CANCELLED;
                result.message = "scan cancelled by progress callback";
                return result;
            }
        }

        Result result;
        result.status = Status::SUCCESS;
        result.message = "scanned source " + source_path;
        return result;
    }
};

std::unique_ptr<IScanner> create_scanner() {
    return std::make_unique<Scanner>();
}

}  // namespace backup
