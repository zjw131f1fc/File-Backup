add_library(backup_restore
    src/restore.cpp
)
target_include_directories(backup_restore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(backup_restore PUBLIC backup_common backup_archive_reader)

add_subdirectory(tests)
