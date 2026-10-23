add_executable(mylog_tests test_mylog.cpp)
target_link_libraries(mylog_tests PRIVATE mylog::mylog)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
add_test(NAME mylog_tests COMMAND mylog_tests)
set_tests_properties(mylog_tests PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

# 独立进程捕获真实控制台文本，同时拒绝任何本应被过滤的正文。
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/console")
add_test(NAME mylog_console COMMAND mylog_tests --console-probe)
set_tests_properties(mylog_console PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/console"
    PASS_REGULAR_EXPRESSION "^inherit:first\r?\nconsole:warning:dedicated\r?\nconsole:debug:lowered\r?\ncustom:formatter\r?\nlocal:override-formatter\r?\nall:unified\r?\nall:still-unified\r?\n\r?\nfinal:restored\r?\n\r?\nconfigured:initial-specific\r?\nconsole-probe-passed"
    FAIL_REGULAR_EXPRESSION "forbidden;CHECK failed"
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
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${suffix}")
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${suffix}"
    )
endforeach()
