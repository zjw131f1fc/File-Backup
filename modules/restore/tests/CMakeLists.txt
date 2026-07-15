add_executable(restore_test restore_test.cpp)
target_link_libraries(restore_test PRIVATE backup_restore backup_archive_writer backup_archive_reader GTest::gtest_main)
target_include_directories(restore_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/helpers)

add_test(NAME restore COMMAND restore_test)
set_tests_properties(restore PROPERTIES LABELS "restore")
