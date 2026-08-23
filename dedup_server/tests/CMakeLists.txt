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
    ${ImageMagick++_INCLUDE_DIRS}
)

# Link directories
link_directories(${GTEST_LIBRARIES})
link_directories(${ImageMagick++_LIBRARY_DIRS})

# Unit Tests
set(UNIT_TEST_SOURCES
    unit/test_unified_observable_config.cpp
    unit/test_console_input_manager.cpp
    unit/test_database_service.cpp
    unit/test_user_settings_service.cpp
    unit/test_scanned_files_ops.cpp
    unit/test_day_groups_ops.cpp
    unit/test_preview_cache_ops.cpp
    unit/test_image_artifacts_ops.cpp
    unit/test_processing_errors_ops.cpp
    unit/test_tpm.cpp
    unit/test_file_scanner.cpp
    unit/test_media_filtering.cpp
    unit/test_disk_cache.cpp
    unit/test_scheduler_service.cpp
    unit/test_files_manager_scheduler.cpp
    unit/test_files_service_immediate_scan.cpp
    unit/test_scheduled_jobs_independence.cpp
    unit/test_property_manager.cpp
    unit/test_media_processor.cpp
    unit/test_image_magick_adapter.cpp
    unit/test_image_magick_transcoder.cpp
    unit/test_raw_file_detector.cpp
    unit/test_transcoding_pipeline.cpp
    unit/test_scheduler_concurrency.cpp
    unit/test_quality_pipeline_memory.cpp
    unit/test_onnx_session_manager.cpp
    unit/test_duplicate_finder_range.cpp
    unit/test_purge_ejected_errored.cpp
    unit/test_duplicates_reset_api.cpp
    unit/test_files_service_change_path.cpp
    unit/test_eject_collision.cpp
    unit/test_grpc_media_service.cpp
    unit/test_magick_args.cpp
    unit/test_magick_limits.cpp
    unit/test_media_generation_failures.cpp
    unit/test_similarity_calculator.cpp
    unit/test_session_pragmas.cpp
    unit/test_vector_index.cpp
    unit/test_clip_tokenizer.cpp
    unit/test_duplicate_group_rank.cpp
    unit/test_exif_reader.cpp
    unit/test_location_geocoder.cpp
    unit/test_exif_query_parser.cpp
    unit/test_exif_keyword_config.cpp
    unit/test_exif_search_e2e.cpp
    unit/test_llm_runtime.cpp
    unit/test_llm_query_structurer.cpp
    unit/test_llm_fact_validator.cpp
    unit/test_display_geometry.cpp
    unit/test_duplicate_members_geometry.cpp
    unit/test_thumbnail_cache_ops.cpp
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
    if(${test_name} STREQUAL "test_processing_errors_ops")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        # Just add the test to the test list without creating executable
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="ProcessingErrorsOpsTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_image_magick_adapter")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        # Just add the test to the test list without creating executable
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="ImageMagickAdapterTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_image_magick_transcoder")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="ImageMagickTranscoderTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_raw_file_detector")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="RawFileDetectorTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_transcoding_pipeline")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="TranscodingPipelineTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_scheduler_concurrency")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="SchedulerConcurrencyTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_quality_pipeline_memory")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="QualityPipelineMemoryTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_onnx_session_manager")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="OnnxSessionManagerTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_magick_args")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="MagickArgsTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_magick_limits")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="MagickLimitsTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_media_generation_failures")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="MediaGenerationFailuresTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_similarity_calculator")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="SimilarityCalculatorTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_session_pragmas")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="SessionPragmasTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_duplicate_group_rank")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="DuplicateGroupRankTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_clip_tokenizer")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="ClipTokenizer*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_vector_index")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="VectorIndexTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_duplicate_finder_range")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="DuplicateFinderRangeTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_duplicates_reset_api")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="DuplicatesResetAPITest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_purge_ejected_errored")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="PurgeEjectedErroredTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_grpc_media_service")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="MediaServiceGrpcTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_files_service_immediate_scan")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="FilesServiceImmediateScanTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_files_service_change_path")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="FilesServiceChangePathTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_scheduled_jobs_independence")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="ScheduledJobsIndependenceTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_disk_cache")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="DiskCacheTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_day_groups_ops")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="DayGroupsOpsTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_preview_cache_ops")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="PreviewCacheOpsTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_exif_search_e2e")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="ExifSearchE2ETest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_exif_keyword_config")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="ExifKeywordConfig*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_llm_fact_validator")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="LlmFactValidatorTest*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_llm_query_structurer")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="QueryFactSchemaTest*:LlmQueryStructurerTest*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_llm_runtime")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="LlmRuntimeTest*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_exif_query_parser")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="ExifQueryParser*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_location_geocoder")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="LocationGeocoderTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_exif_reader")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="ExifReader*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_display_geometry")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="DisplayGeometryTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_duplicate_members_geometry")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="DuplicateMembersGeometryTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_thumbnail_cache_ops")
        # Skip creating standalone executable for this test - it's only part of all_unit_tests
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="ThumbnailCacheOpsTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    elseif(${test_name} STREQUAL "test_media_filtering")
        # Skip creating standalone executable for this test
        add_test(NAME ${test_name} COMMAND all_unit_tests --gtest_filter="MediaFilteringTest.*")
        set_tests_properties(${test_name} PROPERTIES
            OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
            TIMEOUT 300
        )
    else()
    add_executable(${test_name} ${test_source})
    # Add extra sources for specific tests
    if(${test_name} STREQUAL "test_console_input_manager")
        target_sources(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src/core/server_console_input_manager.cpp)
    endif()
    if(${test_name} STREQUAL "test_database_service")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/database/database_service.cpp
            ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/vector_index.cpp
        )
        target_include_directories(${test_name} PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_compile_definitions(${test_name} PRIVATE SQLITE_ENABLE_LOAD_EXTENSION)
    endif()
    if(${test_name} STREQUAL "test_eject_collision")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/vector_index.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/user_settings/user_settings_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/ejected_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/processing_errors_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/duplicate_groups_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/day_groups_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/services/eject_service.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestration/integrity_check_service.cpp
            ${CMAKE_SOURCE_DIR}/src/core/media_backfill_hook.cpp
            ${CMAKE_SOURCE_DIR}/src/orchestration/duplicate_finder.cpp
            ${CMAKE_SOURCE_DIR}/src/orchestration/scheduler_service.cpp
            ${CMAKE_SOURCE_DIR}/src/orchestration/thread_pool_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/similarity/similarity_calculator.cpp
            ${CMAKE_SOURCE_DIR}/src/filesmanager/file_scanner.cpp
            ${CMAKE_SOURCE_DIR}/src/filesmanager/files_service.cpp
            ${CMAKE_SOURCE_DIR}/src/filesmanager/media_availability.cpp
            ${CMAKE_SOURCE_DIR}/src/database/image_artifacts_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/thumbnail_cache_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/preview_cache_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_file_detector.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/heic_adapter.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/tiff_validator.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/subprocess_runner.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/temp_paths.cpp
        )
        target_include_directories(${test_name} PRIVATE ${SQLITE3_INCLUDE_DIRS} ${LIBRAW_INCLUDE_DIRS} ${LIBTIFF_INCLUDE_DIRS})
        target_link_directories(${test_name} PRIVATE ${LIBRAW_LIBRARY_DIRS} ${LIBTIFF_LIBRARY_DIRS})
        target_compile_definitions(${test_name} PRIVATE SQLITE_ENABLE_LOAD_EXTENSION)
    endif()
    if(${test_name} STREQUAL "test_user_settings_service")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/vector_index.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/user_settings/user_settings_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/ejected_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/filesmanager/files_service.cpp
        )
        target_include_directories(${test_name} PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_compile_definitions(${test_name} PRIVATE SQLITE_ENABLE_LOAD_EXTENSION)
    endif()
    if(${test_name} STREQUAL "test_scanned_files_ops")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/vector_index.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/ejected_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/user_settings/user_settings_ops.cpp
        )
        target_include_directories(${test_name} PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_compile_definitions(${test_name} PRIVATE SQLITE_ENABLE_LOAD_EXTENSION)
    endif()
    if(${test_name} STREQUAL "test_image_artifacts_ops")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/image_artifacts_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/vector_index.cpp
        )
        target_include_directories(${test_name} PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_compile_definitions(${test_name} PRIVATE SQLITE_ENABLE_LOAD_EXTENSION)
    endif()
    if(${test_name} STREQUAL "test_processing_errors_ops")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/vector_index.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/processing_errors_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/ejected_files_ops.cpp
        )
        target_include_directories(${test_name} PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_compile_definitions(${test_name} PRIVATE SQLITE_ENABLE_LOAD_EXTENSION)
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
            ${CMAKE_SOURCE_DIR}/src/config/config_property_manager.cpp
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
            ${CMAKE_SOURCE_DIR}/src/database/vector_index.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/ejected_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/processing_errors_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/user_settings/user_settings_ops.cpp
        )
        target_include_directories(${test_name} PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_compile_definitions(${test_name} PRIVATE STANDALONE_MAIN_FILESFM=1 SQLITE_ENABLE_LOAD_EXTENSION)
    endif()
    if(${test_name} STREQUAL "test_file_scanner")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/filesmanager/file_scanner.cpp
        )
        target_compile_definitions(${test_name} PRIVATE FILE_SCANNER_TEST_STANDALONE_MAIN=1)
    endif()
    if(${test_name} STREQUAL "test_media_processor")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/media_processors/media_processor.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image_processor.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/pipeline_thumbnail_helper.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/onnx_adapter.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/onnx_session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/image_magick_adapter.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/image_magick_transcoder.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_file_detector.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/heic_adapter.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/tiff_validator.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/transcoding_pipeline.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/pipelines/quality_pipeline.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/thumbnail_generator.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/subprocess_runner.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/temp_paths.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/tool_paths.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_demosaic_renderer.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_embedded_extractor.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/exif_reader.cpp
            ${CMAKE_SOURCE_DIR}/src/core/media_ready_bus.cpp
            ${CMAKE_SOURCE_DIR}/src/core/media_generation_failures.cpp
            ${CMAKE_SOURCE_DIR}/src/database/image_artifacts_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/vector_index.cpp
            ${CMAKE_SOURCE_DIR}/src/database/thumbnail_cache_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/preview_cache_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/ejected_files_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/processing_errors_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/user_settings/user_settings_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/orchestration/thread_pool_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/filesmanager/disk_cache.cpp
        )
        target_include_directories(${test_name} PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_compile_definitions(${test_name} PRIVATE SQLITE_ENABLE_LOAD_EXTENSION)
    endif()
    if(${test_name} STREQUAL "test_files_manager_scheduler")
        target_sources(${test_name} PRIVATE 
            ${CMAKE_SOURCE_DIR}/src/media_processors/media_processor.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image_processor.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/pipeline_thumbnail_helper.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/onnx_adapter.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/onnx_session_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/image_magick_adapter.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/image_magick_transcoder.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_file_detector.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/heic_adapter.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/tiff_validator.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/transcoding_pipeline.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/pipelines/quality_pipeline.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/thumbnail_generator.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/subprocess_runner.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/temp_paths.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/tool_paths.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_demosaic_renderer.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_embedded_extractor.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/exif_reader.cpp
            ${CMAKE_SOURCE_DIR}/src/core/media_ready_bus.cpp
            ${CMAKE_SOURCE_DIR}/src/core/media_generation_failures.cpp
            ${CMAKE_SOURCE_DIR}/src/database/image_artifacts_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/vector_index.cpp
            ${CMAKE_SOURCE_DIR}/src/database/thumbnail_cache_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/preview_cache_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/processing_errors_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/user_settings/user_settings_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/orchestration/thread_pool_manager.cpp
            ${CMAKE_SOURCE_DIR}/src/filesmanager/disk_cache.cpp
            ${CMAKE_SOURCE_DIR}/src/orchestration/duplicate_finder.cpp
            ${CMAKE_SOURCE_DIR}/src/database/duplicate_groups_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/day_groups_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/media_processors/similarity/similarity_calculator.cpp
        )
        target_include_directories(${test_name} PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_compile_definitions(${test_name} PRIVATE SQLITE_ENABLE_LOAD_EXTENSION)
    endif()
    target_compile_options(${test_name} PRIVATE ${COMMON_TEST_FLAGS})
    target_include_directories(${test_name} PRIVATE
        ${LIBRAW_INCLUDE_DIRS}
        ${LIBTIFF_INCLUDE_DIRS}
    )
    target_link_directories(${test_name} PRIVATE
        ${LIBRAW_LIBRARY_DIRS}
        ${LIBTIFF_LIBRARY_DIRS}
    )
    target_link_libraries(${test_name} 
        ${GTEST_LIBRARIES} 
        pthread
        ${Poco_LIBRARIES}
        ${SQLITE3_LIBRARIES}
        unified_config_lib
        test_utils
        ${OpenCV_LIBS}
        ${ONNXRUNTIME_LIBRARY}
        ${ImageMagick++_LIBRARIES}
        ${LIBRAW_LIBRARIES}
        ${LIBTIFF_LIBRARIES}
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        OUTPUT_FILE "${TEST_OUTPUT_DIRECTORY}/${test_name}.log"
        TIMEOUT 300
    )
    endif()
endforeach()
# Aggregate all unit tests into a single binary
add_executable(all_unit_tests
    unit/all_tests_main.cpp
    ${UNIT_TEST_SOURCES}
)
target_compile_definitions(all_unit_tests PRIVATE ALL_UNIT_TESTS SQLITE_ENABLE_LOAD_EXTENSION)
target_compile_options(all_unit_tests PRIVATE ${COMMON_TEST_FLAGS})

set_source_files_properties(
    ${CMAKE_BINARY_DIR}/generated/dedup.pb.cc
    ${CMAKE_BINARY_DIR}/generated/dedup.grpc.pb.cc
    PROPERTIES GENERATED TRUE
)

target_sources(all_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/server_console_input_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/thumbnail_service.cpp
    ${CMAKE_SOURCE_DIR}/src/services/eject_service.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestration/integrity_check_service.cpp
    ${CMAKE_SOURCE_DIR}/src/core/media_backfill_hook.cpp
    ${CMAKE_SOURCE_DIR}/src/database/database_service.cpp
    ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/database/image_artifacts_ops.cpp
        ${CMAKE_SOURCE_DIR}/src/database/vector_index.cpp
        ${CMAKE_SOURCE_DIR}/src/database/duplicate_groups_ops.cpp
            ${CMAKE_SOURCE_DIR}/src/database/day_groups_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/database/user_settings/user_settings_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/database/ejected_files_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/database/processing_errors_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/database/thumbnail_cache_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/database/preview_cache_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestration/duplicate_finder.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/similarity/similarity_calculator.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/thumbnail_generator.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/subprocess_runner.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/temp_paths.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/tool_paths.cpp
    ${CMAKE_SOURCE_DIR}/src/core/media_ready_bus.cpp
    ${CMAKE_SOURCE_DIR}/src/core/media_generation_failures.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_demosaic_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_embedded_extractor.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/exif_reader.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestration/thread_pool_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestration/scheduler_service.cpp
    ${CMAKE_SOURCE_DIR}/src/orchestration/files_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/filesmanager/file_scanner.cpp
    ${CMAKE_SOURCE_DIR}/src/filesmanager/files_service.cpp
    ${CMAKE_SOURCE_DIR}/src/filesmanager/media_availability.cpp
    ${CMAKE_SOURCE_DIR}/src/database/scanned_files/scanned_files_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/database/ejected_files_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/filesmanager/disk_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/media_processor.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image_processor.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/onnx_adapter.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/onnx_session_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/text/clip_tokenizer.cpp
        ${CMAKE_SOURCE_DIR}/src/search/text_search_engine.cpp
        ${CMAKE_SOURCE_DIR}/src/search/location_geocoder.cpp
        ${CMAKE_SOURCE_DIR}/src/search/exif_query_parser.cpp
        ${CMAKE_SOURCE_DIR}/src/search/exif_keyword_config.cpp
        ${CMAKE_SOURCE_DIR}/src/search/llm_runtime.cpp
        ${CMAKE_SOURCE_DIR}/src/search/query_fact_schema.cpp
        ${CMAKE_SOURCE_DIR}/src/search/llm_query_structurer.cpp
        ${CMAKE_SOURCE_DIR}/src/search/llm_fact_validator.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/image/pipelines/quality_pipeline.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/image/pipeline_thumbnail_helper.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/image_magick_adapter.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/image_magick_transcoder.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_file_detector.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/heic_adapter.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/tiff_validator.cpp
        ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/transcoding_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/heic_adapter.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/tiff_validator.cpp
    ${CMAKE_SOURCE_DIR}/src/core/grpc_server/grpc_server_core.cpp
    ${CMAKE_BINARY_DIR}/generated/dedup.pb.cc
    ${CMAKE_BINARY_DIR}/generated/dedup.grpc.pb.cc
)
target_include_directories(all_unit_tests PRIVATE
    ${LIBRAW_INCLUDE_DIRS}
    ${LIBTIFF_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${CMAKE_BINARY_DIR}/generated
)
target_link_directories(all_unit_tests PRIVATE
    ${LIBRAW_LIBRARY_DIRS}
    ${LIBTIFF_LIBRARY_DIRS}
    ${GRPC_LIBRARY_DIRS}
    ${PROTOBUF_LIBRARY_DIRS}
)
target_link_libraries(all_unit_tests 
    ${GTEST_LIBRARIES}
    llama
    pthread
    ${Poco_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    unified_config_lib
    yaml-cpp::yaml-cpp
    ${LIBRAW_LIBRARIES}
    ${LIBTIFF_LIBRARIES}
    test_utils
    ${OpenCV_LIBS}
    ${ONNXRUNTIME_LIBRARY}
    ${ImageMagick++_LIBRARIES}
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
)
add_dependencies(all_unit_tests setup_test_data)
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

# Test data setup: create build/test_data and copy from tests/test_data if present
# (tests/test_data may be absent when not committed, e.g. in .gitignore)
# Remove and recreate test_data/databases so no leftover .sqlite/.shm/.wal from previous runs (avoids DB lock timeouts)
add_custom_target(setup_test_data
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/test_data
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/test_data/databases
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/test_data/databases
    # The property schema is functional (properties don't exist without it);
    # stage it at build/config/ so tests running from the build dir find it.
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/config/config.schema.json ${CMAKE_BINARY_DIR}/config/config.schema.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/config/config.descriptions.json ${CMAKE_BINARY_DIR}/config/config.descriptions.json
    COMMENT "Setting up test data..."
)
set(TEST_DATA_DEPENDENCY setup_test_data)
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_data")
    add_custom_target(copy_test_data
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/tests/test_data ${CMAKE_BINARY_DIR}/test_data
        COMMENT "Copying test data..."
    )
    add_dependencies(copy_test_data setup_test_data)
    set(TEST_DATA_DEPENDENCY copy_test_data)
endif()

# Make all tests depend on test data setup
foreach(test_source ${UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    if(NOT ${test_name} STREQUAL "test_image_magick_adapter" AND 
       NOT ${test_name} STREQUAL "test_image_magick_transcoder" AND
       NOT ${test_name} STREQUAL "test_raw_file_detector" AND 
       NOT ${test_name} STREQUAL "test_transcoding_pipeline" AND
       NOT ${test_name} STREQUAL "test_scheduler_concurrency" AND
       NOT ${test_name} STREQUAL "test_files_service_immediate_scan" AND
       NOT ${test_name} STREQUAL "test_files_service_change_path" AND
       NOT ${test_name} STREQUAL "test_scheduled_jobs_independence" AND
      NOT ${test_name} STREQUAL "test_quality_pipeline_memory" AND
      NOT ${test_name} STREQUAL "test_onnx_session_manager" AND
      NOT ${test_name} STREQUAL "test_disk_cache" AND
      NOT ${test_name} STREQUAL "test_processing_errors_ops" AND
      NOT ${test_name} STREQUAL "test_duplicate_finder_range" AND
      NOT ${test_name} STREQUAL "test_purge_ejected_errored" AND
      NOT ${test_name} STREQUAL "test_preview_cache_ops" AND
      NOT ${test_name} STREQUAL "test_day_groups_ops" AND
      NOT ${test_name} STREQUAL "test_media_filtering" AND
      NOT ${test_name} STREQUAL "test_eject_collision" AND
      NOT ${test_name} STREQUAL "test_grpc_media_service" AND
      NOT ${test_name} STREQUAL "test_duplicates_reset_api" AND
      NOT ${test_name} STREQUAL "test_magick_args" AND
      NOT ${test_name} STREQUAL "test_magick_limits" AND
      NOT ${test_name} STREQUAL "test_media_generation_failures" AND
      NOT ${test_name} STREQUAL "test_similarity_calculator" AND
      NOT ${test_name} STREQUAL "test_session_pragmas" AND
      NOT ${test_name} STREQUAL "test_vector_index" AND
      NOT ${test_name} STREQUAL "test_clip_tokenizer" AND
      NOT ${test_name} STREQUAL "test_duplicate_group_rank" AND
      NOT ${test_name} STREQUAL "test_exif_reader" AND
      NOT ${test_name} STREQUAL "test_location_geocoder" AND
      NOT ${test_name} STREQUAL "test_exif_query_parser" AND
      NOT ${test_name} STREQUAL "test_exif_keyword_config" AND
      NOT ${test_name} STREQUAL "test_llm_runtime" AND
      NOT ${test_name} STREQUAL "test_llm_query_structurer" AND
      NOT ${test_name} STREQUAL "test_llm_fact_validator" AND
      NOT ${test_name} STREQUAL "test_exif_search_e2e" AND
      NOT ${test_name} STREQUAL "test_display_geometry" AND
      NOT ${test_name} STREQUAL "test_duplicate_members_geometry" AND
      NOT ${test_name} STREQUAL "test_thumbnail_cache_ops")
        add_dependencies(${test_name} ${TEST_DATA_DEPENDENCY})
    endif()
endforeach()

# Test configuration - set properties on test names, not source files
foreach(test_source ${UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    if(NOT ${test_name} STREQUAL "test_image_magick_adapter" AND 
       NOT ${test_name} STREQUAL "test_image_magick_transcoder" AND
       NOT ${test_name} STREQUAL "test_raw_file_detector" AND 
       NOT ${test_name} STREQUAL "test_transcoding_pipeline" AND
       NOT ${test_name} STREQUAL "test_files_service_immediate_scan" AND
       NOT ${test_name} STREQUAL "test_files_service_change_path" AND
       NOT ${test_name} STREQUAL "test_scheduled_jobs_independence" AND
       NOT ${test_name} STREQUAL "test_quality_pipeline_memory" AND
       NOT ${test_name} STREQUAL "test_purge_ejected_errored" AND
       NOT ${test_name} STREQUAL "test_preview_cache_ops" AND
       NOT ${test_name} STREQUAL "test_day_groups_ops" AND
        NOT ${test_name} STREQUAL "test_media_filtering" AND
       NOT ${test_name} STREQUAL "test_eject_collision" AND
       NOT ${test_name} STREQUAL "test_grpc_media_service" AND
       NOT ${test_name} STREQUAL "test_onnx_session_manager")
        set_tests_properties(${test_name} PROPERTIES
            LABELS "unit"
            ENVIRONMENT "TEST_TYPE=unit"
        )
    endif()
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
