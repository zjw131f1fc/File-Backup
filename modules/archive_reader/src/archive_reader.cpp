#include "modules/archive_reader/archive_reader.h"

namespace backup {

class StubArchiveReader : public IArchiveReader {
public:
    explicit StubArchiveReader(const std::string& archive_path)
        : archive_path_(archive_path) {}

    Result validate() override {
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: validated archive " + archive_path_;
        return r;
    }

    bool has_next_entry() override {
        // 桩实现：始终返回 false（没有条目）
        return false;
    }

    Result next_entry(EntryInfo& entry_info) override {
        (void)entry_info;
        Result r;
        r.status = Status::FAILED;
        r.message = "stub: no more entries";
        return r;
    }

    std::unique_ptr<std::istream> open_content(const EntryInfo& entry_info) override {
        (void)entry_info;
        // 桩实现：返回 nullptr
        return nullptr;
    }

private:
    std::string archive_path_;
};

std::unique_ptr<IArchiveReader> open_archive(const std::string& archive_path) {
    return std::make_unique<StubArchiveReader>(archive_path);
}

}  // namespace backup
