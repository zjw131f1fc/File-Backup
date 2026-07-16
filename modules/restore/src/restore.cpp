#include "modules/restore/restore.h"
#include <filesystem>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <vector>
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
        if (part == ".." || part == ".") {
            return false;
        }
    }
    return true;
}

// Validate every existing parent without following a symlink. This protects
// paths such as safe/link/file where link redirects outside the restore root.
bool safe_parent_chain(const std::filesystem::path& root,
                       const std::filesystem::path& relative) {
    auto current = root;
    auto end = relative.end();
    if (relative.begin() != end) {
        --end;
    }
    for (auto it = relative.begin(); it != end; ++it) {
        current /= *it;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (!error && status.type() == std::filesystem::file_type::symlink) {
            return false;
        }
        if (!error && status.type() != std::filesystem::file_type::not_found &&
            status.type() != std::filesystem::file_type::directory) {
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

// Conflict replacement is deliberately centralized so special-file branches
// cannot accidentally use different deletion behavior.
bool remove_existing(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return !error;
}

std::filesystem::path renamed_target(const std::filesystem::path& target) {
    // Keep the archive filename intact and append a deterministic numeric
    // suffix. The upper bound prevents an unbounded search in a crowded folder.
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

class Restorer : public IRestorer {
public:
    Result restore_entry(const std::string& target_root,
                         const EntryInfo& entry_info,
                         IArchiveReader& reader,
                         ConflictPolicy conflict_policy) override {
        if (!safe_relative_path(entry_info.path)) {
            return failed_result("unsafe restore path: " + entry_info.path);
        }

        const std::filesystem::path root(target_root);
        if (!prepare_root(root)) {
            return failed_result("restore root is not a safe directory: " + target_root);
        }
        const std::filesystem::path relative(entry_info.path);
        // A renamed parent changes the physical destination of every child.
        // Resolve that mapping before applying path-safety checks or creating it.
        auto target = resolve_target(root, relative);
        const auto resolved_relative = target.lexically_relative(root);
        if (!safe_relative_path(resolved_relative.generic_string()) ||
            !safe_parent_chain(root, resolved_relative)) {
            return failed_result("unsafe restore parent path: " + entry_info.path);
        }
        const auto original_target = root / relative;
        if (exists_without_following(target)) {
            if (conflict_policy == ConflictPolicy::SKIP) {
                // Remember the skip so the scheduler's later metadata call does
                // not modify the pre-existing object.
                skipped_targets_.insert(path_key(original_target));
                resolved_targets_[path_key(original_target)] = target;
                return success_result("skipped existing " + target.string());
            }
            if (conflict_policy == ConflictPolicy::RENAME) {
                // The mapping is recorded only after creation succeeds, so a
                // failed restore cannot redirect later archive entries.
                target = renamed_target(target);
                if (target.empty()) {
                    return failed_result("failed to choose renamed target: " + entry_info.path);
                }
            } else if (entry_info.type == EntryType::DIRECTORY &&
                       std::filesystem::is_directory(
                           std::filesystem::symlink_status(target))) {
                // Reuse an existing directory. Removing it would also remove
                // unrelated children that are not present in the archive.
            } else if (entry_info.type == EntryType::REGULAR_FILE &&
                       !std::filesystem::is_directory(
                           std::filesystem::symlink_status(target))) {
                // A completed temporary file atomically replaces this target.
            } else if (!remove_existing(target)) {
                return failed_result("failed to remove existing target: " + target.string());
            }
        }

        std::error_code error;
        // Archives may omit directory entries through filtering. Creating the
        // parent chain still allows an included descendant to be restored.
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) {
            return failed_result("failed to create target parent: " + error.message());
        }

        Result result;
        switch (entry_info.type) {
            case EntryType::REGULAR_FILE:
                result = restore_regular_file(target, entry_info, reader);
                break;
            case EntryType::DIRECTORY:
                if (!std::filesystem::create_directory(target, error) && error) {
                    return failed_result("failed to create directory: " + error.message());
                }
                result = success_result("restored directory " + target.string());
                break;
            case EntryType::SYMBOLIC_LINK:
                // Preserve the archived link text. Never resolve or validate it
                // as a filesystem path because dangling links are legitimate.
                if (::symlink(entry_info.link_target.c_str(), target.c_str()) != 0) {
                    return failed_result("failed to create symbolic link: " + target.string());
                }
                result = success_result("restored symbolic link " + target.string());
                break;
            case EntryType::HARD_LINK: {
                // Resolve through an earlier RENAME mapping so both names still
                // reference the same inode under the selected conflict policy.
                if (!safe_relative_path(entry_info.hard_link_target)) {
                    return failed_result("unsafe hard link target: " + entry_info.hard_link_target);
                }
                const auto source = resolve_target(
                    root, std::filesystem::path(entry_info.hard_link_target));
                const auto source_relative = source.lexically_relative(root);
                if (!safe_relative_path(source_relative.generic_string()) ||
                    !safe_parent_chain(root, source_relative)) {
                    return failed_result("unsafe hard link parent: " + entry_info.hard_link_target);
                }
                if (::link(source.c_str(), target.c_str()) != 0) {
                    return failed_result("failed to create hard link: " + target.string());
                }
                result = success_result("restored hard link " + target.string());
                break;
            }
            case EntryType::FIFO:
                if (::mkfifo(target.c_str(), entry_info.permissions & 07777) != 0) {
                    return failed_result("failed to create FIFO: " + target.string());
                }
                result = success_result("restored FIFO " + target.string());
                break;
            case EntryType::CHARACTER_DEVICE:
            case EntryType::BLOCK_DEVICE: {
                // mknod commonly requires elevated privileges. Its failure is
                // returned to the scheduler as an entry-level restore error.
                const mode_t type = entry_info.type == EntryType::CHARACTER_DEVICE
                    ? S_IFCHR : S_IFBLK;
                const auto device = makedev(entry_info.device_major, entry_info.device_minor);
                if (::mknod(target.c_str(), type | (entry_info.permissions & 07777), device) != 0) {
                    return failed_result("failed to create device: " + target.string());
                }
                result = success_result("restored device " + target.string());
                break;
            }
            default:
                return failed_result("unsupported entry type");
        }
        if (!result.ok()) {
            return result;
        }
        // Metadata calls still use the archive path. Keep the actual path here
        // so RENAME and hard links remain transparent to the public interface.
        resolved_targets_[path_key(original_target)] = target;
        refresh_parent_directory_metadata(target.parent_path());
        return result;
    }

    Result restore_metadata(const std::string& target_path,
                            const EntryInfo& entry_info) override {
        const std::filesystem::path requested(target_path);
        if (skipped_targets_.count(path_key(requested)) != 0) {
            return success_result("metadata skipped for existing " + target_path);
        }
        auto target = requested;
        // Scheduler intentionally knows only archive paths. Translate that path
        // here when restore_entry selected a renamed destination.
        const auto resolved = resolved_targets_.find(path_key(requested));
        if (resolved != resolved_targets_.end()) {
            target = resolved->second;
        }
        if (!exists_without_following(target)) {
            return failed_result("target path does not exist: " + target_path);
        }

        Result result = apply_metadata(target, entry_info);
        if (entry_info.type == EntryType::DIRECTORY) {
            // Child creation changes a directory's timestamps. Cache its desired
            // metadata so later restore_entry calls can reapply it.
            directory_metadata_[path_key(target)] = entry_info;
        }
        return result;
    }

private:
    static std::string path_key(const std::filesystem::path& path) {
        return path.lexically_normal().generic_string();
    }

    static bool prepare_root(const std::filesystem::path& root) {
        // Creating a missing root is convenient, but an existing symlink root is
        // rejected because all subsequent lexical containment checks trust it.
        std::error_code error;
        auto status = std::filesystem::symlink_status(root, error);
        if (!error && status.type() == std::filesystem::file_type::symlink) {
            return false;
        }
        if (error || status.type() == std::filesystem::file_type::not_found) {
            error.clear();
            std::filesystem::create_directories(root, error);
            if (error) return false;
            status = std::filesystem::symlink_status(root, error);
        }
        return !error && status.type() == std::filesystem::file_type::directory;
    }

    std::filesystem::path resolve_target(const std::filesystem::path& root,
                                         const std::filesystem::path& relative) const {
        auto original = root;
        auto resolved = root;
        for (const auto& component : relative) {
            original /= component;
            // Use the longest renamed prefix encountered while walking the
            // archive-relative path (for example dir -> dir.1).
            const auto mapped = resolved_targets_.find(path_key(original));
            if (mapped != resolved_targets_.end()) {
                resolved = mapped->second;
            } else {
                resolved /= component;
            }
        }
        return resolved;
    }

    static Result apply_metadata(const std::filesystem::path& target,
                                 const EntryInfo& entry_info) {
        int warnings = 0;
        std::string messages;
        const bool is_link = entry_info.type == EntryType::SYMBOLIC_LINK;
        // lchown and AT_SYMLINK_NOFOLLOW modify the link object rather than the
        // potentially external object referenced by it.
        const int owner_result = is_link
            ? ::lchown(target.c_str(), entry_info.uid, entry_info.gid)
            : ::chown(target.c_str(), entry_info.uid, entry_info.gid);
        if (owner_result != 0) {
            ++warnings;
            messages += "failed to restore owner; ";
        }
        if (!is_link && entry_info.permissions != 0 &&
            ::chmod(target.c_str(), entry_info.permissions & 07777) != 0) {
            ++warnings;
            messages += "failed to restore permissions; ";
        }
        if (entry_info.atime_sec != 0 || entry_info.mtime_sec != 0) {
            struct timespec times[2] = {
                {entry_info.atime_sec, entry_info.atime_nsec},
                {entry_info.mtime_sec, entry_info.mtime_nsec},
            };
            if (::utimensat(AT_FDCWD, target.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
                ++warnings;
                messages += "failed to restore timestamps; ";
            }
        }
        Result result = success_result("restored metadata for " + target.string());
        // Metadata restoration is best effort by contract. Preserve SUCCESS so
        // valid content is not reported as lost, and expose warning_count.
        result.warning_count = warnings;
        if (warnings != 0) {
            result.message = messages + target.string();
        }
        return result;
    }

    void refresh_parent_directory_metadata(std::filesystem::path parent) {
        // Restore cached ancestors after creating a child. This preserves final
        // directory timestamps without adding a new scheduler lifecycle hook.
        while (!parent.empty()) {
            const auto found = directory_metadata_.find(path_key(parent));
            if (found != directory_metadata_.end()) {
                apply_metadata(parent, found->second);
            }
            const auto next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
    }

    static Result success_result(const std::string& message) {
        Result result;
        result.status = Status::SUCCESS;
        result.message = message;
        return result;
    }

    static Result restore_regular_file(const std::filesystem::path& target,
                                       const EntryInfo& entry_info,
                                       IArchiveReader& reader) {
        auto content = reader.open_content(entry_info);
        if (!content) {
            return failed_result("archive content is unavailable: " + entry_info.path);
        }

        std::string temporary_template = target.string() + ".restore.XXXXXX";
        // mkstemp provides a unique file in the destination directory, avoiding
        // collisions between concurrent restore tasks.
        std::vector<char> temporary_name(temporary_template.begin(), temporary_template.end());
        temporary_name.push_back('\0');
        const int temporary_fd = ::mkstemp(temporary_name.data());
        if (temporary_fd < 0) {
            return failed_result("failed to create temporary target: " +
                                 std::string(std::strerror(errno)));
        }
        const std::filesystem::path temporary(temporary_name.data());
        std::array<char, 64 * 1024> buffer {};
        uint64_t written_bytes = 0;
        bool failed = false;
        while (*content) {
            // Handle short reads, short writes, and EINTR explicitly. A stream
            // insertion operator would not let us validate the byte count.
            content->read(buffer.data(), buffer.size());
            const std::streamsize count = content->gcount();
            std::streamsize offset = 0;
            while (offset < count) {
                const ssize_t written = ::write(temporary_fd, buffer.data() + offset,
                                                static_cast<size_t>(count - offset));
                if (written < 0) {
                    if (errno == EINTR) continue;
                    failed = true;
                    break;
                }
                offset += written;
                written_bytes += static_cast<uint64_t>(written);
            }
            if (failed) break;
        }
        if (content->bad() || written_bytes != entry_info.size) failed = true;
        // A size mismatch usually means a truncated/corrupt archive. Never
        // install that partial file over a valid destination.
        if (::close(temporary_fd) != 0) failed = true;
        if (failed) {
            std::filesystem::remove(temporary);
            return failed_result("failed to write complete temporary target: " + temporary.string());
        }

        // The temporary file is in the target directory, so rename is atomic
        // on POSIX filesystems and preserves an existing file on write failure.
        std::error_code error;
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary);
            return failed_result("failed to install target: " + error.message());
        }
        return success_result("restored regular file " + target.string());
    }

    std::map<std::string, std::filesystem::path> resolved_targets_;
    std::set<std::string> skipped_targets_;
    std::map<std::string, EntryInfo> directory_metadata_;
};

std::unique_ptr<IRestorer> create_restorer() {
    return std::make_unique<Restorer>();
}

}  // namespace backup
