# Tests CMakeLists.txt for Media Deduplication Server

# Enable testing
enable_testing()

# Find Google Test
find_package(GTest REQUIRED)

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
    unit/test_observable_property.cpp
    unit/test_log_level.cpp
    unit/test_observable_log_level.cpp
    unit/test_observable_config_manager.cpp
    unit/test_config_manager.cpp
    unit/test_database_manager.cpp
    unit/test_server.cpp
)

# Integration Tests
set(INTEGRATION_TEST_SOURCES
    integration/test_config_file_sync.cpp
    integration/test_property_change_callbacks.cpp
    integration/test_file_monitoring.cpp
    integration/test_config_validation.cpp
)

# Performance Tests
set(PERFORMANCE_TEST_SOURCES
    performance/test_property_access_performance.cpp
    performance/test_file_monitoring_performance.cpp
    performance/test_callback_performance.cpp
)

# Mock Objects
set(MOCK_SOURCES
    mocks/mock_file_system.cpp
    mocks/mock_logger.cpp
    mocks/mock_config_storage.cpp
)

# Create test executables
foreach(test_source ${UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_compile_options(${test_name} PRIVATE ${COMMON_TEST_FLAGS})
    target_link_libraries(${test_name} 
        ${GTEST_LIBRARIES} 
        pthread
        ${Poco_LIBRARIES}
        sqlite3
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
        TIMEOUT 300
    )
endforeach()

foreach(test_source ${INTEGRATION_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_compile_options(${test_name} PRIVATE ${COMMON_TEST_FLAGS})
    target_link_libraries(${test_name} 
        ${GTEST_LIBRARIES} 
        pthread
        ${Poco_LIBRARIES}
        sqlite3
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
        TIMEOUT 600
    )
endforeach()

foreach(test_source ${PERFORMANCE_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_compile_options(${test_name} PRIVATE ${COMMON_TEST_FLAGS})
    target_link_libraries(${test_name} 
        ${GTEST_LIBRARIES} 
        pthread
        ${Poco_LIBRARIES}
        sqlite3
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
        TIMEOUT 1200
    )
endforeach()

# Create mock library
add_library(test_mocks STATIC ${MOCK_SOURCES})
target_compile_options(test_mocks PRIVATE ${COMMON_TEST_FLAGS})
target_link_libraries(test_mocks ${Poco_LIBRARIES})

# Test utilities
add_library(test_utils STATIC
    test_utils.cpp
    test_helpers.cpp
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

add_custom_target(run_integration_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --tests-regex "test_.*"
    DEPENDS ${INTEGRATION_TEST_SOURCES}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running integration tests..."
)

add_custom_target(run_performance_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --tests-regex "test_.*"
    DEPENDS ${PERFORMANCE_TEST_SOURCES}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running performance tests..."
)

add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS ${UNIT_TEST_SOURCES} ${INTEGRATION_TEST_SOURCES} ${PERFORMANCE_TEST_SOURCES}
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
foreach(test_source ${UNIT_TEST_SOURCES} ${INTEGRATION_TEST_SOURCES} ${PERFORMANCE_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_dependencies(${test_name} setup_test_data)
endforeach()

# Test configuration
set_tests_properties(${UNIT_TEST_SOURCES} PROPERTIES
    LABELS "unit"
    ENVIRONMENT "TEST_TYPE=unit"
)

set_tests_properties(${INTEGRATION_TEST_SOURCES} PROPERTIES
    LABELS "integration"
    ENVIRONMENT "TEST_TYPE=integration"
)

set_tests_properties(${PERFORMANCE_TEST_SOURCES} PROPERTIES
    LABELS "performance"
    ENVIRONMENT "TEST_TYPE=performance"
)

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
message(STATUS "  Integration tests: ${INTEGRATION_TEST_SOURCES}")
message(STATUS "  Performance tests: ${PERFORMANCE_TEST_SOURCES}")
message(STATUS "  Test output directory: ${TEST_OUTPUT_DIRECTORY}")
message(STATUS "  Google Test found: ${GTEST_FOUND}")
