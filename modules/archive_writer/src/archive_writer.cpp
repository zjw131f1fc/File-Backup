#include "modules/archive_writer/archive_writer.h"

namespace backup {

class StubArchiveWriter : public IArchiveWriter {
public:
    explicit StubArchiveWriter(const std::string& output_path)
        : output_path_(output_path) {}

    Result add_entry(
        const EntryInfo& entry_info,
        std::istream& content
    ) override {
        (void)content;
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: added entry with content " + entry_info.path;
        return r;
    }

    Result add_entry(const EntryInfo& entry_info) override {
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: added entry " + entry_info.path;
        return r;
    }

    Result commit() override {
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: committed archive to " + output_path_;
        return r;
    }

    Result abort() override {
        Result r;
        r.status = Status::SUCCESS;
        r.message = "stub: aborted archive " + output_path_;
        return r;
    }

private:
    std::string output_path_;
};

std::unique_ptr<IArchiveWriter> create_archive(const std::string& output_path) {
    return std::make_unique<StubArchiveWriter>(output_path);
}

}  // namespace backup
