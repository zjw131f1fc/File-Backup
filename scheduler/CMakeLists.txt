add_library(backup_scheduler_lib
    src/task_manager.cpp
    src/task_runtime.cpp
    src/task_runtime_submission.cpp
    src/task_runtime_events.cpp
    src/task_runtime_worker.cpp
    src/backup_scheduler.cpp
    src/restore_scheduler.cpp
)
target_include_directories(backup_scheduler_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(backup_scheduler_lib PUBLIC
    backup_common
    backup_scanner
    backup_filter
    backup_archive_writer
    backup_archive_reader
    backup_restore
)

add_subdirectory(tests)
