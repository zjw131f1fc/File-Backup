#include "modules/restore/restore.h"
#include "modules/archive_reader/archive_reader.h"
#include <filesystem>

namespace backup {

namespace {

Result failed_result(const std::string& message) {
    Result r;
    r.status = Status::FAILED;
    r.message = message;
    return r;
}

}  // namespace

class StubRestorer : public IRestorer {
public:
    Result restore_entry(
        const std::string& target_root,
        const EntryInfo& entry_info,
        IArchiveReader& reader,
        ConflictPolicy conflict_policy
    ) override {
        (void)reader;
        (void)conflict_policy;
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: restored " + entry_info.path + " to " + target_root;
        return r;
    }

    Result restore_metadata(
        const std::string& target_path,
        const EntryInfo& entry_info
    ) override {
        (void)entry_info;
        std::error_code error;
        const auto target_status = std::filesystem::symlink_status(target_path, error);
        if (error || target_status.type() == std::filesystem::file_type::not_found) {
            return failed_result("target path does not exist: " + target_path);
        }

        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: accepted metadata for " + target_path;
        return r;
    }
};

std::unique_ptr<IRestorer> create_restorer() {
    return std::make_unique<StubRestorer>();
}

}  // namespace backup
