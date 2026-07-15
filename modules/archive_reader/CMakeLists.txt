add_library(backup_archive_reader
    src/archive_reader.cpp
)
target_include_directories(backup_archive_reader PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(backup_archive_reader PUBLIC backup_common)

add_subdirectory(tests)
