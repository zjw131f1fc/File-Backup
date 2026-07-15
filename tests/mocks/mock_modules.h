#pragma once

#include "modules/scanner/scanner.h"
#include "modules/filter/filter.h"
#include "modules/archive_writer/archive_writer.h"
#include "modules/archive_reader/archive_reader.h"
#include "modules/restore/restore.h"
#include <gmock/gmock.h>

namespace backup::testing {

class MockScanner : public IScanner {
public:
    MOCK_METHOD(Result, scan_and_backup,
        (const std::string& source_path,
         IFilter& filter,
         IArchiveWriter& archive_writer,
         ProgressCallback progress_callback),
        (override));
};

class MockFilter : public IFilter {
public:
    MOCK_METHOD(bool, should_include, (const EntryInfo& entry), (override));
};

class MockArchiveWriter : public IArchiveWriter {
public:
    MOCK_METHOD(Result, add_entry,
        (const EntryInfo& entry_info, std::istream& content), (override));
    MOCK_METHOD(Result, add_entry, (const EntryInfo& entry_info), (override));
    MOCK_METHOD(Result, commit, (), (override));
    MOCK_METHOD(Result, abort, (), (override));
};

class MockArchiveReader : public IArchiveReader {
public:
    MOCK_METHOD(Result, validate, (), (override));
    MOCK_METHOD(bool, has_next_entry, (), (override));
    MOCK_METHOD(Result, next_entry, (EntryInfo& entry_info), (override));
    MOCK_METHOD(std::unique_ptr<std::istream>, open_content,
        (const EntryInfo& entry_info), (override));
};

class MockRestorer : public IRestorer {
public:
    MOCK_METHOD(Result, restore_entry,
        (const std::string& target_root,
         const EntryInfo& entry_info,
         IArchiveReader& reader,
         ConflictPolicy conflict_policy),
        (override));
    MOCK_METHOD(Result, restore_metadata,
        (const std::string& target_path,
         const EntryInfo& entry_info),
        (override));
};

}  // namespace backup::testing
