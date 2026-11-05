add_executable(storq_core_tests test_core.cpp)
target_link_libraries(storq_core_tests PRIVATE Storq::core)
set_target_properties(storq_core_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME storq_core_tests COMMAND storq_core_tests)

add_executable(storq_storage_tests test_storage.cpp)
target_link_libraries(storq_storage_tests PRIVATE Storq::core)
set_target_properties(storq_storage_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME storq_storage_tests COMMAND storq_storage_tests)

add_executable(storq_format_tests test_format.cpp)
target_link_libraries(storq_format_tests PRIVATE Storq::core)
set_target_properties(storq_format_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(storq_format_tests PRIVATE SQLite3::SQLite3)
    else()
        target_link_libraries(storq_format_tests PRIVATE SQLite::SQLite3)
    endif()
    target_compile_definitions(storq_format_tests PRIVATE STORQ_TEST_SQLITE3=1)
endif()
add_test(NAME storq_format_tests COMMAND storq_format_tests)

add_executable(storq_analysis_tests test_analysis.cpp)
target_link_libraries(storq_analysis_tests PRIVATE Storq::core)
set_target_properties(storq_analysis_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(SQLite3_FOUND)
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(storq_analysis_tests PRIVATE SQLite3::SQLite3)
    else()
        target_link_libraries(storq_analysis_tests PRIVATE SQLite::SQLite3)
    endif()
    target_compile_definitions(storq_analysis_tests PRIVATE STORQ_TEST_SQLITE3=1)
endif()
add_test(NAME storq_analysis_tests COMMAND storq_analysis_tests)
