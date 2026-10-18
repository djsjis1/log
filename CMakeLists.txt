# mylog 顶层构建入口：用于独立构建库 + 示例程序
# 库本体（含 spdlog 依赖）位于 mylog/ 子目录，可整个拷贝到其他项目：
#   add_subdirectory(path/to/mylog)
#   target_link_libraries(your_target PRIVATE mylog::mylog)

cmake_minimum_required(VERSION 3.11)

project(mylog CXX)

# ------------------ 选项 ------------------
option(MYLOG_BUILD_EXAMPLE "Build mylog example" ON)

# ------------------ 库 ------------------
add_subdirectory(mylog)

# ------------------ 示例 ------------------
if(MYLOG_BUILD_EXAMPLE)
    add_executable(mylog_example example.cpp)
    target_link_libraries(mylog_example PRIVATE mylog::mylog)
endif()
 