add_executable(task_manager_test task_manager_test.cpp)
target_link_libraries(task_manager_test PRIVATE backup_scheduler_lib GTest::gtest_main)

add_test(NAME task_manager COMMAND task_manager_test)
set_tests_properties(task_manager PROPERTIES LABELS "scheduler")

add_executable(backup_scheduler_test backup_scheduler_test.cpp)
target_link_libraries(backup_scheduler_test PRIVATE backup_scheduler_lib GTest::gtest GTest::gmock_main)
target_include_directories(backup_scheduler_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/mocks)

add_test(NAME backup_scheduler COMMAND backup_scheduler_test)
set_tests_properties(backup_scheduler PROPERTIES LABELS "scheduler")

add_executable(restore_scheduler_test restore_scheduler_test.cpp)
target_link_libraries(restore_scheduler_test PRIVATE backup_scheduler_lib GTest::gtest GTest::gmock_main)
target_include_directories(restore_scheduler_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/mocks)

add_test(NAME restore_scheduler COMMAND restore_scheduler_test)
set_tests_properties(restore_scheduler PROPERTIES LABELS "scheduler")

add_executable(task_runtime_test task_runtime_test.cpp)
target_link_libraries(task_runtime_test PRIVATE backup_scheduler_lib GTest::gtest_main)

add_test(NAME task_runtime COMMAND task_runtime_test)
set_tests_properties(task_runtime PROPERTIES LABELS "scheduler")
