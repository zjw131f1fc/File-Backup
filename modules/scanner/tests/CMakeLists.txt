add_executable(scanner_test scanner_test.cpp)
target_link_libraries(scanner_test PRIVATE backup_scanner backup_filter backup_archive_writer backup_archive_reader GTest::gtest_main)
target_include_directories(scanner_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/helpers)

add_test(NAME scanner COMMAND scanner_test)
set_tests_properties(scanner PROPERTIES LABELS "scanner")
