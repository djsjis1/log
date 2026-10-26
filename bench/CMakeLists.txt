# 基准程序：README「性能」一节数据的复现入口。
# 默认不构建；顶层配置时加 -DMYLOG_BUILD_BENCH=ON 启用。
# 绑核仅 Windows 生效（affinity.h）；其他平台数值只可看比值，见 README「测量口径」。

set(MYLOG_BENCH_PROGRAMS bench opt gap scaling fair_same_time)

foreach(name IN LISTS MYLOG_BENCH_PROGRAMS)
    add_executable(mylog_${name} ${name}.cpp)
    target_link_libraries(mylog_${name} PRIVATE mylog::mylog)
    # target 名带前缀避免与目录/其他 target 混淆，可执行文件保持原名
    set_target_properties(mylog_${name} PROPERTIES OUTPUT_NAME ${name})
    target_include_directories(mylog_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(MSVC)
        target_compile_options(mylog_${name} PRIVATE /utf-8 /W4)
    else()
        target_compile_options(mylog_${name} PRIVATE -Wall -Wextra)
    endif()
    if(MYLOG_WERROR)
        if(MSVC)
            target_compile_options(mylog_${name} PRIVATE /WX)
        else()
            target_compile_options(mylog_${name} PRIVATE -Werror)
        endif()
    endif()
endforeach()
