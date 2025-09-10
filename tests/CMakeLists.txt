# Tests CMakeLists.txt for Media Deduplication Server

# Enable testing
enable_testing()

# Find Google Test
find_package(GTest REQUIRED)

# Find yaml-cpp
find_package(yaml-cpp REQUIRED)

# Set test output directory
set(TEST_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_results)

# Common test flags
set(COMMON_TEST_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -std=c++17
    -O2
    -g
)

# Test include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${GTEST_INCLUDE_DIRS}
)

# Link directories
link_directories(${GTEST_LIBRARIES})

# Unit Tests
set(UNIT_TEST_SOURCES
    unit/test_unified_observable_config.cpp
    unit/test_console_input_manager.cpp
    unit/test_database_service.cpp
    unit/test_user_settings_service.cpp
    unit/test_scanned_files_ops.cpp
    unit/test_tpm.cpp
    unit/test_file_scanner.cpp
    unit/test_scheduler_service.cpp
    unit/test_files_manager_scheduler.cpp
    unit/test_property_manager.cpp
)

# Integration Tests - REMOVED for brevity
set(INTEGRATION_TEST_SOURCES
)

# Performance Tests - REMOVED for brevity
set(PERFORMANCE_TEST_SOURCES
)

# Mock Objects
set(MOCK_SOURCES
    # Placeholder for future mock objects
)

# Create test executables
foreach(test_source ${UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    # Add extra sources for specific tests
    if(${test_name} STREQUAL "test_console_input_manager")
        target_sources(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src/core/console_input_manager.cpp)
    endif()
    if(${test_name} STREQUAL "test_database_service")
        target_sources(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src/database/database_service.cpp)
    endif()
    if(${test_name} STREQUAL "test_user_settings_service")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_lease.cpp
            ${CMAKE_SOURCE_DIR}/src/database/user_settings/user_settings_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/filesmanager/files_service.cpp
        )
    endif()
    if(${test_name} STREQUAL "test_scanned_files_ops")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_lease.cpp
            ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
        )
    endif()
    if(${test_name} STREQUAL "test_tpm")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/orchestration/thread_pool_manager.cpp
        )
    endif()
    if(${test_name} STREQUAL "test_scheduler_service")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/orchestration/scheduler_service.cpp
            ${CMAKE_SOURCE_DIR}/src/orchestration/thread_pool_manager.cpp
        )
        target_compile_definitions(${test_name} PRIVATE STANDALONE_MAIN_SCHED=1)
    endif()
    if(${test_name} STREQUAL "test_property_manager")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/config/property_manager.cpp
        )
    endif()
    if(${test_name} STREQUAL "test_files_manager_scheduler")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/orchestration/scheduler_service.cpp
            ${CMAKE_SOURCE_DIR}/src/orchestration/thread_pool_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/orchestration/files_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/filesmanager/files_service.cpp
            ${CMAKE_SOURCE_DIR}/src/filesmanager/file_scanner.cpp
            ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_lease.cpp
            ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/user_settings/user_settings_ops.cpp
        )
        target_compile_definitions(${test_name} PRIVATE STANDALONE_MAIN_FILESFM=1)
    endif()
    if(${test_name} STREQUAL "test_file_scanner")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/filesmanager/file_scanner.cpp
        )
        target_compile_definitions(${test_name} PRIVATE FILE_SCANNER_TEST_STANDALONE_MAIN=1)
    endif()
    target_compile_options(${test_name} PRIVATE ${COMMON_TEST_FLAGS})
    target_link_libraries(${test_name} 
        ${GTEST_LIBRARIES} 
        pthread
        ${Poco_LIBRARIES}
        sqlite3
        unified_config_lib
        test_utils
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
        TIMEOUT 300
    )
endforeach()
# Aggregate all unit tests into a single binary
add_executable(all_unit_tests
    unit/all_tests_main.cpp
    ${UNIT_TEST_SOURCES}
)
target_compile_definitions(all_unit_tests PRIVATE ALL_UNIT_TESTS)
target_compile_options(all_unit_tests PRIVATE ${COMMON_TEST_FLAGS})
target_sources(all_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/console_input_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/database/database_service.cpp
    ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/database/session/session_lease.cpp
    ${CMAKE_SOURCE_DIR}/src/database/user_settings/user_settings_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestration/thread_pool_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestration/scheduler_service.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestration/files_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/filesmanager/file_scanner.cpp
    ${CMAKE_SOURCE_DIR}/src/filesmanager/files_service.cpp
)
target_link_libraries(all_unit_tests 
    ${GTEST_LIBRARIES}
    pthread
    ${Poco_LIBRARIES}
    sqlite3
    unified_config_lib
    yaml-cpp::yaml-cpp
    test_utils
)
add_test(NAME all_unit_tests COMMAND all_unit_tests)
set_tests_properties(all_unit_tests PROPERTIES
    OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/all_unit_tests.log"
    TIMEOUT 600
)

# Integration and Performance test loops removed for brevity

# Create mock library (only if mock sources exist)
if(MOCK_SOURCES)
    add_library(test_mocks STATIC ${MOCK_SOURCES})
    target_compile_options(test_mocks PRIVATE ${COMMON_TEST_FLAGS})
    target_link_libraries(test_mocks ${Poco_LIBRARIES})
endif()

# Test utilities
add_library(test_utils STATIC
    test_utils.cpp
)
target_compile_options(test_utils PRIVATE ${COMMON_TEST_FLAGS})
target_link_libraries(test_utils ${Poco_LIBRARIES})

# Custom test targets
add_custom_target(run_unit_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --tests-regex "test_.*"
    DEPENDS ${UNIT_TEST_SOURCES}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running unit tests..."
)

# Integration and Performance test targets removed for brevity

add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS ${UNIT_TEST_SOURCES}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all tests..."
)

# Test data setup
add_custom_target(setup_test_data
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/test_data
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/tests/test_data ${CMAKE_BINARY_DIR}/test_data
    COMMENT "Setting up test data..."
)

# Make all tests depend on test data setup
foreach(test_source ${UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_dependencies(${test_name} setup_test_data)
endforeach()

# Test configuration - set properties on test names, not source files
foreach(test_source ${UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    set_tests_properties(${test_name} PROPERTIES
        LABELS "unit"
        ENVIRONMENT "TEST_TYPE=unit"
    )
endforeach()

# Integration and Performance test property loops removed for brevity

# Coverage support (if available)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    find_package(codecov QUIET)
    if(codecov_FOUND)
        add_custom_target(coverage
            COMMAND ${codecov_EXECUTABLE} --root ${CMAKE_SOURCE_DIR} --build-root ${CMAKE_BINARY_DIR}
            DEPENDS run_all_tests
            COMMENT "Generating coverage report..."
        )
    endif()
endif()

# Print test configuration
message(STATUS "Test configuration:")
message(STATUS "  Unit tests: ${UNIT_TEST_SOURCES}")
message(STATUS "  Integration tests: REMOVED for brevity")
message(STATUS "  Performance tests: REMOVED for brevity")
message(STATUS "  Test output directory: ${TEST_OUTPUT_DIRECTORY}")
message(STATUS "  Google Test found: ${GTEST_FOUND}")
