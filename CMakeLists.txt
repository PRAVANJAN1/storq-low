cmake_minimum_required(VERSION 3.20)

project(Storq VERSION 0.1.0 LANGUAGES CXX)

option(STORQ_BUILD_SHARED "Build Storq as a shared library" OFF)
option(STORQ_BUILD_EXAMPLES "Build Storq examples" ON)
option(STORQ_BUILD_TESTS "Build Storq tests" ON)

if(CMAKE_CONFIGURATION_TYPES)
    set(STORQ_BUILD_TYPE_VALUE "$<CONFIG>")
elseif(CMAKE_BUILD_TYPE)
    set(STORQ_BUILD_TYPE_VALUE "${CMAKE_BUILD_TYPE}")
else()
    set(STORQ_BUILD_TYPE_VALUE "Unknown")
endif()

set(STORQ_CORE_SOURCES
    src/common/utilities.cpp
    src/analysis/page_analyzer.cpp
    src/core/capabilities.cpp
    src/core/configuration.cpp
    src/core/error.cpp
    src/core/logger.cpp
    src/core/module_registry.cpp
    src/core/runtime.cpp
    src/core/version.cpp
    src/crash/crash_simulator.cpp
    src/recovery/recovery_engine.cpp
    src/forensics/forensics_engine.cpp
    src/format/format_inspector.cpp
    src/fault/fault_injector.cpp
    src/storage/storage.cpp
    src/wal/wal_analyzer.cpp
)

if(STORQ_BUILD_SHARED)
    add_library(storq_core SHARED ${STORQ_CORE_SOURCES})
else()
    add_library(storq_core STATIC ${STORQ_CORE_SOURCES})
endif()

add_library(Storq::core ALIAS storq_core)
set_target_properties(storq_core PROPERTIES EXPORT_NAME core)

target_compile_features(storq_core PUBLIC cxx_std_20)
target_include_directories(storq_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_definitions(storq_core
    PRIVATE
        STORQ_BUILD_TYPE="${STORQ_BUILD_TYPE_VALUE}"
)
find_package(Threads REQUIRED)
target_link_libraries(storq_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(storq_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(storq_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(STORQ_BUILD_SHARED AND WIN32)
    set_target_properties(storq_core PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if(STORQ_BUILD_EXAMPLES)
    add_executable(storq_basic_runtime examples/basic_runtime.cpp)
    target_link_libraries(storq_basic_runtime PRIVATE Storq::core)
    set_target_properties(storq_basic_runtime PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_storage examples/storage.cpp)
    target_link_libraries(storq_storage PRIVATE Storq::core)
    set_target_properties(storq_storage PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_format_inspector examples/format_inspector.cpp)
    target_link_libraries(storq_format_inspector PRIVATE Storq::core)
    set_target_properties(storq_format_inspector PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_page_analyzer examples/page_analyzer.cpp)
    target_link_libraries(storq_page_analyzer PRIVATE Storq::core)
    set_target_properties(storq_page_analyzer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_analyze_malformed examples/analyze_malformed.cpp)
    target_link_libraries(storq_analyze_malformed PRIVATE Storq::core)
    set_target_properties(storq_analyze_malformed PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_wal_analyzer examples/wal_analyzer.cpp)
    target_link_libraries(storq_wal_analyzer PRIVATE Storq::core)
    set_target_properties(storq_wal_analyzer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_analyze_truncated_wal examples/analyze_truncated_wal.cpp)
    target_link_libraries(storq_analyze_truncated_wal PRIVATE Storq::core)
    set_target_properties(storq_analyze_truncated_wal PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_fault_injection examples/fault_injection.cpp)
    target_link_libraries(storq_fault_injection PRIVATE Storq::core)
    set_target_properties(storq_fault_injection PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_corrupt_wal examples/corrupt_wal.cpp)
    target_link_libraries(storq_corrupt_wal PRIVATE Storq::core)
    set_target_properties(storq_corrupt_wal PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_crash_simulation examples/crash_simulation.cpp)
    target_link_libraries(storq_crash_simulation PRIVATE Storq::core)
    set_target_properties(storq_crash_simulation PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_recovery_verification examples/recovery_verification.cpp)
    target_link_libraries(storq_recovery_verification PRIVATE Storq::core)
    set_target_properties(storq_recovery_verification PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_executable(storq_forensics examples/forensics.cpp)
    target_link_libraries(storq_forensics PRIVATE Storq::core)
    set_target_properties(storq_forensics PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()

if(STORQ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS storq_core
    EXPORT StorqTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT StorqTargets
    FILE StorqTargets.cmake
    NAMESPACE Storq::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Storq
)
configure_package_config_file(
    cmake/StorqConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/StorqConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Storq
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/StorqConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/StorqConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/StorqConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Storq
)
