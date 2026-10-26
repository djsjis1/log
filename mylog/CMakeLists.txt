# mylog - 基于 spdlog 的轻量日志封装（可嵌入子模块）
# 在其他项目中引入方式：
#   add_subdirectory(path/to/mylog)
#   target_link_libraries(your_target PRIVATE mylog::mylog)
# 业务代码只需: #include "Logger.h" 并使用 LOG_INFO(...) 等宏。
# 本目录自包含：Logger.h/Logger.cpp + spdlog 源码，可直接整个拷贝到其他项目。

cmake_minimum_required(VERSION 3.21)

# 允许独立构建，也允许被 add_subdirectory 引入
if(NOT DEFINED PROJECT_NAME)
    project(mylog CXX)
endif()

# ------------------ 依赖: spdlog ------------------
# vendored spdlog 源码目录：升级版本时改这里（目录名本身也带版本号）。
set(MYLOG_SPDLOG_SUBDIR "spdlog-1.17.0")

# 如果外层已经引入 spdlog，则复用；否则自动引入本目录下的 spdlog 源码
if(NOT TARGET spdlog::spdlog AND NOT TARGET spdlog)
    # 关闭 spdlog 自己的示例/测试，减少构建负担
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH   OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL       OFF CACHE BOOL "" FORCE)

    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/${MYLOG_SPDLOG_SUBDIR}
        ${CMAKE_CURRENT_BINARY_DIR}/spdlog-build
        EXCLUDE_FROM_ALL
    )
endif()

# ------------------ 封装静态库 ------------------
add_library(mylog STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Logger.h
)

# 给上层暴露 C++17 要求
target_compile_features(mylog PUBLIC cxx_std_17)

# MSVC 默认按本地编码（GBK）读源文件，而 Logger.cpp 里有中文注释，
# 不指定 UTF-8 会出现乱码或 C4819 警告。用 PRIVATE：只影响本库自身编译。
if(MSVC)
    target_compile_options(mylog PRIVATE /utf-8 /W4)
else()
    target_compile_options(mylog PRIVATE -Wall -Wextra -Wpedantic)
endif()

# MYLOG_WERROR 由外层工程（或 CI）打开：库本体保持零告警不倒退。
if(MYLOG_WERROR)
    if(MSVC)
        target_compile_options(mylog PRIVATE /WX)
    else()
        target_compile_options(mylog PRIVATE -Werror)
    endif()
endif()

# 头文件搜索路径对下游可见
target_include_directories(mylog
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接 spdlog（兼容两种 target 名字）
if(TARGET spdlog::spdlog)
    target_link_libraries(mylog PUBLIC spdlog::spdlog)
elseif(TARGET spdlog)
    target_link_libraries(mylog PUBLIC spdlog)
endif()

# 打开编译期最低日志级别（影响 spdlog 自己的 SPDLOG_LOGGER_XXX 宏）。
# 本封装只调用运行期接口，没有使用那些宏，所以设成 PRIVATE：
# 不把这条定义传染给使用方，避免和对方自己的 SPDLOG_ACTIVE_LEVEL 冲突。
target_compile_definitions(mylog PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

# 方便以别名方式引用
add_library(mylog::mylog ALIAS mylog)
