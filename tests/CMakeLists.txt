add_executable(mylog_tests test_mylog.cpp)
target_link_libraries(mylog_tests PRIVATE mylog::mylog)

add_test(NAME mylog_tests COMMAND mylog_tests)
set_tests_properties(mylog_tests PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
