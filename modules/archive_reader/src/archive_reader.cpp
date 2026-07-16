#include "modules/archive_reader/archive_reader.h"
#include "common/stub_archive_format.h"
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace backup {

namespace {

Result failed_result(const std::string& message) {
    Result result;
    result.status = Status::FAILED;
    result.message = message;
    return result;
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
        if (header != stub_archive::kHeader) {
            return failed_result("invalid stub archive header");
        }

        std::vector<stub_archive::Record> records;
        while (true) {
            uint32_t magic = 0;
            if (!stub_archive::read_value(input, magic)) {
                return failed_result("truncated stub archive");
            }
            if (magic == stub_archive::kEndMagic) {
                break;
            }
            if (magic != stub_archive::kRecordMagic) {
                return failed_result("invalid stub archive record");
            }

            stub_archive::Record record;
            if (!stub_archive::read_record_body(input, record) ||
                !stub_archive::is_safe_path(record.entry.path)) {
                return failed_result("invalid stub archive entry");
            }
            records.push_back(std::move(record));
        }

        records_ = std::move(records);
        next_index_ = 0;
        last_index_.reset();
        validated_ = true;
        Result result;
        result.status = Status::SUCCESS;
        result.message = "stub: validated archive " + archive_path_;
        return result;
    }

    bool has_next_entry() override {
        return validated_ && next_index_ < records_.size();
    }

    Result next_entry(EntryInfo& entry_info) override {
        entry_info = EntryInfo{};
        if (!validated_) {
            return failed_result("archive must be validated before reading entries");
        }
        if (!has_next_entry()) {
            return failed_result("stub archive has no more entries");
        }

        last_index_ = next_index_;
        entry_info = records_[next_index_].entry;
        ++next_index_;
        Result result;
        result.status = Status::SUCCESS;
        result.message = "stub: read entry " + entry_info.path;
        return result;
    }

    std::unique_ptr<std::istream> open_content(const EntryInfo& entry_info) override {
        if (!validated_ || !last_index_.has_value() ||
            entry_info.type != EntryType::REGULAR_FILE ||
            *last_index_ >= records_.size() ||
            records_[*last_index_].entry.path != entry_info.path ||
            !records_[*last_index_].has_content) {
            return nullptr;
        }
        return std::make_unique<std::istringstream>(records_[*last_index_].content);
    }

private:
    std::string archive_path_;
    std::vector<stub_archive::Record> records_;
    std::size_t next_index_ = 0;
    std::optional<std::size_t> last_index_;
    bool validated_ = false;
};

std::unique_ptr<IArchiveReader> open_archive(const std::string& archive_path) {
    return std::make_unique<StubArchiveReader>(archive_path);
}

}  // namespace backup
