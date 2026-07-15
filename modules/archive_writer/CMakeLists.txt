add_library(backup_archive_writer
    src/archive_writer.cpp
)
target_include_directories(backup_archive_writer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(backup_archive_writer PUBLIC backup_common)

add_subdirectory(tests)
