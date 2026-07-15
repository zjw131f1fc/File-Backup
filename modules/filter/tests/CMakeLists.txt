add_executable(filter_test filter_test.cpp)
target_link_libraries(filter_test PRIVATE backup_filter GTest::gtest_main)

add_test(NAME filter COMMAND filter_test)
set_tests_properties(filter PROPERTIES LABELS "filter")
