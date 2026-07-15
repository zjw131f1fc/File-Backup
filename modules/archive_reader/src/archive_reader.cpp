#include "modules/archive_reader/archive_reader.h"
#include <filesystem>
#include <fstream>

namespace backup {

namespace {

constexpr char kStubArchiveHeader[] = "BACKUP_STUB_ARCHIVE_V1";

Result failed_result(const std::string& message) {
    Result r;
    r.status = Status::FAILED;
    r.message = message;
    return r;
}

}  // namespace

class StubArchiveReader : public IArchiveReader {
public:
    explicit StubArchiveReader(const std::string& archive_path)
        : archive_path_(archive_path) {}

    Result validate() override {
        std::ifstream input(archive_path_, std::ios::binary);
        if (!input) {
            return failed_result("failed to open archive: " + archive_path_);
        }

        std::string header;
        std::getline(input, header);
        if (header != kStubArchiveHeader) {
            return failed_result("invalid stub archive header");
        }

        validated_ = true;
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: validated archive " + archive_path_;
        return r;
    }

    bool has_next_entry() override {
        return validated_ && has_entry_;
    }

    Result next_entry(EntryInfo& entry_info) override {
        entry_info = EntryInfo{};
        if (!validated_) {
            return failed_result("archive must be validated before reading entries");
        }
        return failed_result("stub archive has no more entries");
    }

    std::unique_ptr<std::istream> open_content(const EntryInfo& entry_info) override {
        (void)entry_info;
        return nullptr;
    }

private:
    std::string archive_path_;
    bool validated_ = false;
    bool has_entry_ = false;
};

std::unique_ptr<IArchiveReader> open_archive(const std::string& archive_path) {
    return std::make_unique<StubArchiveReader>(archive_path);
}

}  // namespace backup
