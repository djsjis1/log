# mylog 顶层构建入口：用于独立构建库 + 示例程序
# 库本体（含 spdlog 依赖）位于 mylog/ 子目录，可整个拷贝到其他项目：
#   add_subdirectory(path/to/mylog)
#   target_link_libraries(your_target PRIVATE mylog::mylog)
#
# 版本必须与 mylog/CMakeLists.txt 一致：子目录要求 3.21，顶层声明更低版本时
# 3.11~3.20 的 CMake 会在这里配置失败。

cmake_minimum_required(VERSION 3.21)

project(mylog CXX)

# ------------------ 选项 ------------------
option(MYLOG_BUILD_EXAMPLE "Build mylog example" ON)
option(MYLOG_BUILD_TESTS "Build mylog tests" ON)
option(MYLOG_BUILD_BENCH "Build benchmarks in bench/ (README 性能数据的来源)" OFF)
# 把告警当错误，守住“零告警”不倒退。由使用方/CI 决定是否开启。
option(MYLOG_WERROR "Treat compiler warnings as errors (mylog/example/tests/bench)" OFF)

# ------------------ 库 ------------------
add_subdirectory(mylog)

# ------------------ 示例 ------------------
if(MYLOG_BUILD_EXAMPLE)
    add_executable(mylog_example example.cpp)
    target_link_libraries(mylog_example PRIVATE mylog::mylog)
    if(MSVC)
        target_compile_options(mylog_example PRIVATE /utf-8 /W4)
    else()
        target_compile_options(mylog_example PRIVATE -Wall -Wextra)
    endif()
    if(MYLOG_WERROR)
        if(MSVC)
            target_compile_options(mylog_example PRIVATE /WX)
        else()
            target_compile_options(mylog_example PRIVATE -Werror)
        endif()
    endif()
endif()

# ------------------ 基准 ------------------
if(MYLOG_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# ------------------ 测试 ------------------
if(MYLOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
