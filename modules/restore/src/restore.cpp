#include "modules/restore/restore.h"
#include "modules/archive_reader/archive_reader.h"

namespace backup {

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
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: restored metadata for " + target_path;
        return r;
    }
};

std::unique_ptr<IRestorer> create_restorer() {
    return std::make_unique<StubRestorer>();
}

}  // namespace backup
