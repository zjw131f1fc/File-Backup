cmake_minimum_required(VERSION 3.14)
project(backup_system VERSION 0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出编译警告
add_compile_options(-Wall -Wextra -Wpedantic)

# 使用系统安装的 gtest/gmock
find_package(GTest REQUIRED)

enable_testing()

add_subdirectory(third_party)
add_subdirectory(common)
add_subdirectory(modules)
add_subdirectory(scheduler)
