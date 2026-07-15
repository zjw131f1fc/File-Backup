add_executable(filter_test filter_test.cpp)
target_link_libraries(filter_test PRIVATE backup_filter GTest::gtest_main)

add_test(NAME filter COMMAND filter_test)
set_tests_properties(filter PROPERTIES LABELS "filter")

add_executable(filter_functional_test filter_functional_test.cpp)
target_link_libraries(filter_functional_test PRIVATE backup_filter GTest::gtest_main)

add_test(NAME filter_functional COMMAND filter_functional_test)
set_tests_properties(filter_functional PROPERTIES LABELS "filter")
