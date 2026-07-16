#include "modules/archive_writer/archive_writer.h"
#include "common/stub_archive_format.h"
#include <filesystem>
#include <fstream>
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

class StubArchiveWriter : public IArchiveWriter {
public:
    explicit StubArchiveWriter(const std::string& output_path)
        : output_path_(output_path)
        , temp_path_(output_path + ".stub.tmp") {}

    Result add_entry(const EntryInfo& entry_info, std::istream& content) override {
        if (!active_) {
            return failed_result("archive writer is no longer active");
        }

        std::ostringstream buffer;
        buffer << content.rdbuf();
        if (!content.good() && !content.eof()) {
            return failed_result("failed to read entry content: " + entry_info.path);
        }

        stub_archive::Record record;
        record.entry = entry_info;
        record.content = buffer.str();
        record.has_content = true;
        records_.push_back(std::move(record));

        Result result;
        result.status = Status::SUCCESS;
        result.message = "stub: accepted entry with content " + entry_info.path;
        return result;
    }

    Result add_entry(const EntryInfo& entry_info) override {
        if (!active_) {
            return failed_result("archive writer is no longer active");
        }

        stub_archive::Record record;
        record.entry = entry_info;
        records_.push_back(std::move(record));

        Result result;
        result.status = Status::SUCCESS;
        result.message = "stub: accepted entry " + entry_info.path;
        return result;
    }

    Result commit() override {
        if (!active_) {
            return failed_result("archive writer is no longer active");
        }

        std::ofstream output(temp_path_, std::ios::binary | std::ios::trunc);
        if (!output || !stub_archive::write_archive(output, records_)) {
            std::filesystem::remove(temp_path_);
            return failed_result("failed to write temporary archive: " + temp_path_);
        }
        output.close();

        std::error_code error;
        std::filesystem::rename(temp_path_, output_path_, error);
        if (error) {
            std::filesystem::remove(temp_path_);
            return failed_result("failed to commit archive: " + error.message());
        }

        active_ = false;
        Result result;
        result.status = Status::SUCCESS;
        result.message = "stub: committed archive to " + output_path_;
        return result;
    }

    Result abort() override {
        if (!active_) {
            return failed_result("archive writer is no longer active");
        }

        std::error_code error;
        std::filesystem::remove(temp_path_, error);
        if (error) {
            return failed_result("failed to abort archive: " + error.message());
        }

        active_ = false;
        Result result;
        result.status = Status::SUCCESS;
        result.message = "stub: aborted archive " + output_path_;
        return result;
    }

private:
    std::string output_path_;
    std::string temp_path_;
    std::vector<stub_archive::Record> records_;
    bool active_ = true;
};

std::unique_ptr<IArchiveWriter> create_archive(const std::string& output_path) {
    return std::make_unique<StubArchiveWriter>(output_path);
}

}  // namespace backup
