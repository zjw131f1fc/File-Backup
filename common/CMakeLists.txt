add_library(backup_common INTERFACE)
target_include_directories(backup_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_subdirectory(tests)
