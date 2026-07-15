add_library(backup_filter
    src/filter.cpp
)
target_include_directories(backup_filter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(backup_filter PUBLIC backup_common)

add_subdirectory(tests)
