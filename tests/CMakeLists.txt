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

add_executable(storq_wal_tests test_wal.cpp)
target_link_libraries(storq_wal_tests PRIVATE Storq::core)
set_target_properties(storq_wal_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(SQLite3_FOUND)
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(storq_wal_tests PRIVATE SQLite3::SQLite3)
    else()
        target_link_libraries(storq_wal_tests PRIVATE SQLite::SQLite3)
    endif()
    target_compile_definitions(storq_wal_tests PRIVATE STORQ_TEST_SQLITE3=1)
endif()
add_test(NAME storq_wal_tests COMMAND storq_wal_tests)

add_executable(storq_fault_tests test_fault.cpp)
target_link_libraries(storq_fault_tests PRIVATE Storq::core)
set_target_properties(storq_fault_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME storq_fault_tests COMMAND storq_fault_tests)

add_executable(storq_crash_tests test_crash.cpp)
target_link_libraries(storq_crash_tests PRIVATE Storq::core)
set_target_properties(storq_crash_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME storq_crash_tests COMMAND storq_crash_tests)

add_executable(storq_recovery_tests test_recovery.cpp)
target_link_libraries(storq_recovery_tests PRIVATE Storq::core)
set_target_properties(storq_recovery_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME storq_recovery_tests COMMAND storq_recovery_tests)

add_executable(storq_forensics_tests test_forensics.cpp)
target_link_libraries(storq_forensics_tests PRIVATE Storq::core)
set_target_properties(storq_forensics_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME storq_forensics_tests COMMAND storq_forensics_tests)

add_executable(storq_adapter_tests test_adapters.cpp)
target_link_libraries(storq_adapter_tests PRIVATE Storq::core)
set_target_properties(storq_adapter_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME storq_adapter_tests COMMAND storq_adapter_tests)

add_executable(storq_workload_tests test_workload.cpp)
target_link_libraries(storq_workload_tests PRIVATE Storq::core)
set_target_properties(storq_workload_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME storq_workload_tests COMMAND storq_workload_tests)
