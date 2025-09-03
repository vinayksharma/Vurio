# Tests directory CMakeLists.txt
# This file organizes the test structure

# Enable testing
enable_testing()

# Find Google Test (optional for now)
# find_package(GTest REQUIRED)

# Test sources
set(TEST_SOURCES
    config/config_manager_test.cpp
    database/database_manager_test.cpp
    core/server_test.cpp
)

# Create test executable
# add_executable(media_dedup_tests ${TEST_SOURCES})
# target_link_libraries(media_dedup_tests
#     Poco::Foundation
#     Poco::Data
#     Poco::DataSQLite
#     Poco::Util
#     ${SQLITE3_LIBRARIES}
#     GTest::GTest
#     GTest::Main
# )

# Add tests
# add_test(NAME ConfigManagerTest COMMAND media_dedup_tests --gtest_filter=ConfigManagerTest.*)
# add_test(NAME DatabaseManagerTest COMMAND media_dedup_tests --gtest_filter=DatabaseManagerTest.*)
# add_test(NAME ServerTest COMMAND media_dedup_tests --gtest_filter=ServerTest.*)

# For now, just create the test structure without enabling tests
message(STATUS "Test structure created - tests can be enabled later by uncommenting the test code")
