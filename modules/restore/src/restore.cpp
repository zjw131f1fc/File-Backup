#include "modules/restore/restore.h"
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

namespace backup {

namespace {

Result failed_result(const std::string& message) {
    Result result;
    result.status = Status::FAILED;
    result.message = message;
    return result;
}

bool safe_relative_path(const std::string& path) {
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

bool exists_without_following(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && status.type() != std::filesystem::file_type::not_found;
}

bool remove_existing(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return !error;
}

std::filesystem::path renamed_target(const std::filesystem::path& target) {
    for (int suffix = 1; suffix < 10000; ++suffix) {
        auto candidate = target;
        candidate += "." + std::to_string(suffix);
        if (!exists_without_following(candidate)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace

class StubRestorer : public IRestorer {
public:
    Result restore_entry(const std::string& target_root,
                         const EntryInfo& entry_info,
                         IArchiveReader& reader,
                         ConflictPolicy conflict_policy) override {
        if (!safe_relative_path(entry_info.path)) {
            return failed_result("unsafe restore path: " + entry_info.path);
        }

        const std::filesystem::path root(target_root);
        auto target = root / entry_info.path;
        if (exists_without_following(target)) {
            if (conflict_policy == ConflictPolicy::SKIP) {
                return success_result("stub: skipped existing " + target.string());
            }
            if (conflict_policy == ConflictPolicy::RENAME) {
                target = renamed_target(target);
                if (target.empty()) {
                    return failed_result("failed to choose renamed target: " + entry_info.path);
                }
            } else if (!remove_existing(target)) {
                return failed_result("failed to remove existing target: " + target.string());
            }
        }

        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) {
            return failed_result("failed to create target parent: " + error.message());
        }

        switch (entry_info.type) {
            case EntryType::REGULAR_FILE:
                return restore_regular_file(target, entry_info, reader);
            case EntryType::DIRECTORY:
                if (!std::filesystem::create_directory(target, error) && error) {
                    return failed_result("failed to create directory: " + error.message());
                }
                return apply_permissions(target, entry_info.permissions);
            case EntryType::SYMBOLIC_LINK:
                if (::symlink(entry_info.link_target.c_str(), target.c_str()) != 0) {
                    return failed_result("failed to create symbolic link: " + target.string());
                }
                return success_result("stub: restored symbolic link " + target.string());
            case EntryType::HARD_LINK: {
                if (!safe_relative_path(entry_info.hard_link_target)) {
                    return failed_result("unsafe hard link target: " + entry_info.hard_link_target);
                }
                const auto source = root / entry_info.hard_link_target;
                if (::link(source.c_str(), target.c_str()) != 0) {
                    return failed_result("failed to create hard link: " + target.string());
                }
                return success_result("stub: restored hard link " + target.string());
            }
            case EntryType::FIFO:
                if (::mkfifo(target.c_str(), entry_info.permissions & 07777) != 0) {
                    return failed_result("failed to create FIFO: " + target.string());
                }
                return success_result("stub: restored FIFO " + target.string());
            case EntryType::CHARACTER_DEVICE:
            case EntryType::BLOCK_DEVICE: {
                const mode_t type = entry_info.type == EntryType::CHARACTER_DEVICE
                    ? S_IFCHR : S_IFBLK;
                const auto device = makedev(entry_info.device_major, entry_info.device_minor);
                if (::mknod(target.c_str(), type | (entry_info.permissions & 07777), device) != 0) {
                    return failed_result("failed to create device: " + target.string());
                }
                return success_result("stub: restored device " + target.string());
            }
        }
        return failed_result("unsupported entry type");
    }

    Result restore_metadata(const std::string& target_path,
                            const EntryInfo& entry_info) override {
        const std::filesystem::path target(target_path);
        if (!exists_without_following(target)) {
            return failed_result("target path does not exist: " + target_path);
        }

        Result permissions_result = apply_permissions(target, entry_info.permissions);
        if (!permissions_result.ok()) {
            return permissions_result;
        }
        if (entry_info.atime_sec == 0 && entry_info.mtime_sec == 0) {
            return success_result("stub: accepted metadata for " + target_path);
        }

        struct timespec times[2] = {
            {entry_info.atime_sec, entry_info.atime_nsec},
            {entry_info.mtime_sec, entry_info.mtime_nsec},
        };
        if (::utimensat(AT_FDCWD, target.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
            return failed_result("failed to update timestamps: " + target_path);
        }
        return success_result("stub: accepted metadata for " + target_path);
    }

private:
    static Result success_result(const std::string& message) {
        Result result;
        result.status = Status::SUCCESS;
        result.message = message;
        return result;
    }

    static Result apply_permissions(const std::filesystem::path& target, mode_t permissions) {
        if (permissions != 0 && ::chmod(target.c_str(), permissions & 07777) != 0) {
            return failed_result("failed to set permissions: " + target.string());
        }
        return success_result("stub: restored metadata for " + target.string());
    }

    static Result restore_regular_file(const std::filesystem::path& target,
                                       const EntryInfo& entry_info,
                                       IArchiveReader& reader) {
        auto content = reader.open_content(entry_info);
        if (!content) {
            return failed_result("archive content is unavailable: " + entry_info.path);
        }

        auto temporary = target;
        temporary += ".stub.tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return failed_result("failed to create temporary target: " + temporary.string());
        }
        output << content->rdbuf();
        output.close();
        if (!output) {
            std::filesystem::remove(temporary);
            return failed_result("failed to write temporary target: " + temporary.string());
        }

        std::error_code error;
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary);
            return failed_result("failed to install target: " + error.message());
        }
        return apply_permissions(target, entry_info.permissions);
    }
};

std::unique_ptr<IRestorer> create_restorer() {
    return std::make_unique<StubRestorer>();
}

}  // namespace backup
