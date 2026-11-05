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
