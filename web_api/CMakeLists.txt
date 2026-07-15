add_library(backup_web_api
    src/web_api.cpp
)
target_include_directories(backup_web_api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(backup_web_api PUBLIC
    backup_scheduler_lib
    httplib::httplib
    nlohmann_json::nlohmann_json
)

add_executable(backup_web_server
    src/main.cpp
)
target_link_libraries(backup_web_server PRIVATE backup_web_api)

add_subdirectory(tests)
