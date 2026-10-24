# 测试不引入第三方框架：只链接标准库 + mylog，由 CTest 驱动。
# 超时做成缓存变量：TSan 等插桩构建会让并发用例慢一个数量级，CI 里需要单独放大。
set(MYLOG_TEST_TIMEOUT 180 CACHE STRING "Timeout (seconds) for the full mylog_tests run")
set(MYLOG_PROBE_TIMEOUT 30 CACHE STRING "Timeout (seconds) for the short probe runs")

add_executable(mylog_tests test_mylog.cpp)
target_link_libraries(mylog_tests PRIVATE mylog::mylog)

if(MSVC)
    target_compile_options(mylog_tests PRIVATE /W4)
else()
    target_compile_options(mylog_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
add_test(NAME mylog_tests COMMAND mylog_tests)
set_tests_properties(mylog_tests PROPERTIES
    TIMEOUT ${MYLOG_TEST_TIMEOUT}
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

# 真实控制台输出由测试进程自己重定向抓取并逐项断言，CTest 只确认一行哨兵。
# 这样调整 pattern/格式不会再让这条用例因为“正则不匹配”而红。
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/console")
add_test(NAME mylog_console COMMAND mylog_tests --console-probe)
set_tests_properties(mylog_console PROPERTIES
    TIMEOUT ${MYLOG_PROBE_TIMEOUT}
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/console"
    PASS_REGULAR_EXPRESSION "console-probe-passed"
    FAIL_REGULAR_EXPRESSION "CHECK failed"
)

# 同一源文件只切换测试入口，不新增源码；编译定义只作用于相应调用目标。
foreach(level IN ITEMS INFO OFF)
    string(TOLOWER "${level}" suffix)
    set(test_target "mylog_tests_${suffix}")
    add_executable(${test_target} test_mylog.cpp)
    target_link_libraries(${test_target} PRIVATE mylog::mylog)
    target_compile_definitions(${test_target} PRIVATE
        MYLOG_COMPILE_LEVEL_TEST=1
        MYLOG_ACTIVE_LEVEL=MYLOG_LEVEL_${level}
    )
    if(MSVC)
        target_compile_options(${test_target} PRIVATE /W4)
    else()
        target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${suffix}")
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES
        TIMEOUT ${MYLOG_PROBE_TIMEOUT}
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${suffix}"
    )
endforeach()
