add_executable(dedup_tests
    status_test.cpp
    auth_test.cpp
    file_utils_test.cpp
    media_processor_test.cpp
    file_processor_test.cpp
    database_manager_test.cpp
    ../src/auth.cpp
    ../src/file_utils.cpp
    ../src/media_processor.cpp
    ../src/database_manager.cpp
    ../src/file_processor.cpp
    ../src/server_config_manager.cpp
)

target_include_directories(dedup_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    $<TARGET_PROPERTY:spdlog::spdlog,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(dedup_tests PRIVATE
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    PkgConfig::SQLITE3
)

include(GoogleTest)
gtest_discover_tests(dedup_tests) 

add_executable(media_processing_orchestrator_test
    media_processing_orchestrator_test.cpp
    ../src/database_manager.cpp
    ../src/media_processing_orchestrator.cpp
    ../src/server_config_manager.cpp
    ../src/media_processor.cpp
    ../src/file_utils.cpp
)
target_link_libraries(media_processing_orchestrator_test
    PRIVATE
    gtest
    gtest_main
    pthread
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::SQLITE3
)
add_test(NAME MediaProcessingOrchestratorTest COMMAND media_processing_orchestrator_test) 