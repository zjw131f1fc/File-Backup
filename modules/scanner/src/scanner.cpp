#include "modules/scanner/scanner.h"
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

class StubScanner : public IScanner {
public:
    Result scan_and_backup(
        const std::string& source_path,
        IFilter& filter,
        IArchiveWriter& archive_writer,
        ProgressCallback progress_callback
    ) override {
        (void)filter;
        (void)archive_writer;

        std::error_code error;
        const auto source_status = std::filesystem::status(source_path, error);
        if (error || !std::filesystem::exists(source_status)) {
            return failed_result("source path does not exist: " + source_path);
        }
        if (!std::filesystem::is_directory(source_status)) {
            return failed_result("source path is not a directory: " + source_path);
        }

        Progress progress;
        progress.stage = "scanning";
        progress.current_path = source_path;
        if (progress_callback && !progress_callback(progress)) {
            Result r;
            r.status = Status::CANCELLED;
            r.message = "stub scan cancelled by progress callback";
            return r;
        }

        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub scanner: accepted source " + source_path + "; no entries written";
        return r;
    }
};

std::unique_ptr<IScanner> create_scanner() {
    return std::make_unique<StubScanner>();
}

}  // namespace backup
