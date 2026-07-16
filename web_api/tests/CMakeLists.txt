add_executable(web_api_test web_api_test.cpp)
target_link_libraries(web_api_test PRIVATE backup_web_api GTest::gtest_main)

add_test(NAME web_api COMMAND web_api_test)
set_tests_properties(web_api PROPERTIES LABELS "web_api")
