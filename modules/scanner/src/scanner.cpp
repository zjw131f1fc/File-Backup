#include "modules/scanner/scanner.h"

namespace backup {

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
        (void)progress_callback;
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub scanner: scanned " + source_path;
        return r;
    }
};

std::unique_ptr<IScanner> create_scanner() {
    return std::make_unique<StubScanner>();
}

}  // namespace backup
