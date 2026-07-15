add_executable(common_test common_test.cpp)
target_link_libraries(common_test PRIVATE backup_common GTest::gtest_main)

add_test(NAME common COMMAND common_test)
set_tests_properties(common PROPERTIES LABELS "common")
