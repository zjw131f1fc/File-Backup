#include "modules/archive_writer/archive_writer.h"
#include <filesystem>
#include <fstream>

namespace backup {

namespace {

constexpr char kStubArchiveHeader[] = "BACKUP_STUB_ARCHIVE_V1\n";

Result failed_result(const std::string& message) {
    Result r;
    r.status = Status::FAILED;
    r.message = message;
    return r;
}

}  // namespace

class StubArchiveWriter : public IArchiveWriter {
public:
    explicit StubArchiveWriter(const std::string& output_path)
        : output_path_(output_path)
        , temp_path_(output_path + ".stub.tmp") {}

    Result add_entry(
        const EntryInfo& entry_info,
        std::istream& content
    ) override {
        if (!active_) {
            return failed_result("archive writer is no longer active");
        }
        (void)content;
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: accepted entry with content " + entry_info.path;
        return r;
    }

    Result add_entry(const EntryInfo& entry_info) override {
        if (!active_) {
            return failed_result("archive writer is no longer active");
        }
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: accepted entry " + entry_info.path;
        return r;
    }

    Result commit() override {
        if (!active_) {
            return failed_result("archive writer is no longer active");
        }

        std::ofstream output(temp_path_, std::ios::binary | std::ios::trunc);
        if (!output) {
            return failed_result("failed to create temporary archive: " + temp_path_);
        }
        output << kStubArchiveHeader;
        output.close();
        if (!output) {
            std::filesystem::remove(temp_path_);
            return failed_result("failed to write temporary archive: " + temp_path_);
        }

        std::error_code error;
        std::filesystem::rename(temp_path_, output_path_, error);
        if (error) {
            std::filesystem::remove(temp_path_);
            return failed_result("failed to commit archive: " + error.message());
        }

        active_ = false;
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: committed archive to " + output_path_;
        return r;
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
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: aborted archive " + output_path_;
        return r;
    }

private:
    std::string output_path_;
    std::string temp_path_;
    bool active_ = true;
};

std::unique_ptr<IArchiveWriter> create_archive(const std::string& output_path) {
    return std::make_unique<StubArchiveWriter>(output_path);
}

}  // namespace backup
