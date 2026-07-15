add_library(backup_scanner
    src/scanner.cpp
)
target_include_directories(backup_scanner PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(backup_scanner PUBLIC backup_common backup_archive_writer backup_filter)

add_subdirectory(tests)
