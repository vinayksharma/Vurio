cmake_minimum_required(VERSION 3.16)
project(MediaDedupServer VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Find Poco packages
find_package(Poco REQUIRED COMPONENTS Foundation Data DataSQLite Util Net JSON)

# On macOS, set Poco paths if not found by find_package
if(APPLE)
    if(NOT Poco_FOUND)
        set(POCO_ROOT "/opt/homebrew")
        if(EXISTS "${POCO_ROOT}/include/Poco")
            set(Poco_INCLUDE_DIRS "${POCO_ROOT}/include")
            set(Poco_LIBRARY_DIRS "${POCO_ROOT}/lib")
            message(STATUS "Using Poco from Homebrew: ${POCO_ROOT}")
        endif()
    endif()
endif()

# Find SQLite3 (prefer Homebrew on macOS so load_extension is available for vector index)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
if(APPLE)
    find_path(SQLITE3_HB_INCLUDE NAMES sqlite3.h PATHS /opt/homebrew/opt/sqlite/include /opt/homebrew/include NO_DEFAULT_PATH)
    find_library(SQLITE3_HB_LIB NAMES sqlite3 PATHS /opt/homebrew/opt/sqlite/lib /opt/homebrew/lib NO_DEFAULT_PATH)
    if(SQLITE3_HB_INCLUDE AND SQLITE3_HB_LIB)
        set(SQLITE3_INCLUDE_DIRS ${SQLITE3_HB_INCLUDE})
        set(SQLITE3_LIBRARIES ${SQLITE3_HB_LIB})
        message(STATUS "Using Homebrew SQLite for load_extension support: ${SQLITE3_HB_LIB}")
    endif()
endif()

# Find gRPC and Protobuf
pkg_check_modules(GRPC REQUIRED grpc++)
pkg_check_modules(PROTOBUF REQUIRED protobuf)

find_program(PROTOC_EXECUTABLE protoc)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)

if(NOT PROTOC_EXECUTABLE OR NOT GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "protoc or grpc_cpp_plugin not found!")
endif()

set(PROTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/api)
set(PROTO_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_BUILD_DIR})

add_custom_command(
    OUTPUT ${PROTO_BUILD_DIR}/dedup.pb.cc ${PROTO_BUILD_DIR}/dedup.pb.h
           ${PROTO_BUILD_DIR}/dedup.grpc.pb.cc ${PROTO_BUILD_DIR}/dedup.grpc.pb.h
    COMMAND ${PROTOC_EXECUTABLE}
    ARGS --cpp_out=${PROTO_BUILD_DIR}
         --grpc_out=${PROTO_BUILD_DIR}
         --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
         -I ${PROTO_SRC_DIR}
         ${PROTO_SRC_DIR}/dedup.proto
    DEPENDS ${PROTO_SRC_DIR}/dedup.proto
)
include_directories(${PROTO_BUILD_DIR})

# Find yaml-cpp
find_package(yaml-cpp REQUIRED)

# Find ImageMagick++
find_package(PkgConfig REQUIRED)
pkg_check_modules(ImageMagick++ REQUIRED Magick++)

# Set ImageMagick++ compile definitions
add_definitions(-DMAGICKCORE_QUANTUM_DEPTH=16)
add_definitions(-DMAGICKCORE_HDRI_ENABLE=1)

# Find LibRaw for RAW file validation
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBRAW REQUIRED libraw)
if(LIBRAW_FOUND)
    message(STATUS "LibRaw found: ${LIBRAW_VERSION}")
    message(STATUS "LibRaw libraries: ${LIBRAW_LIBRARIES}")
    message(STATUS "LibRaw library dirs: ${LIBRAW_LIBRARY_DIRS}")
endif()

# Find libtiff for TIFF file validation
pkg_check_modules(LIBTIFF REQUIRED libtiff-4)
if(LIBTIFF_FOUND)
    message(STATUS "libtiff found: ${LIBTIFF_VERSION}")
endif()

find_package(OpenCV QUIET)
if(OpenCV_FOUND)
    message(STATUS "OpenCV found: ${OpenCV_VERSION}")
    include_directories(${OpenCV_INCLUDE_DIRS})
    add_definitions(-DHAVE_OPENCV)
else()
    message(STATUS "OpenCV not found - image pipelines will use stubs")
endif()

# Optional ONNX Runtime (generic, cross-platform friendly)
# Allow user to provide ONNXRUNTIME_ROOT (var or env) or rely on common prefixes
if(NOT ONNXRUNTIME_ROOT AND DEFINED ENV{ONNXRUNTIME_ROOT})
    set(ONNXRUNTIME_ROOT $ENV{ONNXRUNTIME_ROOT})
endif()

set(_onnx_hints
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
    ${ONNXRUNTIME_ROOT}/include
    ${ONNXRUNTIME_ROOT}
    /opt/homebrew/include
    /usr/local/include
    /usr/include
    ${CMAKE_PREFIX_PATH}
)
set(_onnx_lib_hints
    ${ONNXRUNTIME_ROOT}/lib
    ${ONNXRUNTIME_ROOT}
    /opt/homebrew/lib
    /usr/local/lib
    /usr/lib
    ${CMAKE_PREFIX_PATH}
)

find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime/core/session/onnxruntime_cxx_api.h
    HINTS ${_onnx_hints}
    PATH_SUFFIXES include)
find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime onnxruntime.1 libonnxruntime
    HINTS ${_onnx_lib_hints})

# Fallback manual detection on Apple/Homebrew if not found
if(NOT ONNXRUNTIME_INCLUDE_DIR)
    foreach(prefix /opt/homebrew /usr/local /usr)
        if(EXISTS "${prefix}/include/onnxruntime/core/session/onnxruntime_cxx_api.h")
            set(ONNXRUNTIME_INCLUDE_DIR "${prefix}/include")
            break()
        endif()
    endforeach()
endif()
if(NOT ONNXRUNTIME_LIBRARY)
    foreach(prefix /opt/homebrew /usr/local /usr)
        if(EXISTS "${prefix}/lib/libonnxruntime.dylib")
            set(ONNXRUNTIME_LIBRARY "${prefix}/lib/libonnxruntime.dylib")
            break()
        elseif(EXISTS "${prefix}/lib/libonnxruntime.so")
            set(ONNXRUNTIME_LIBRARY "${prefix}/lib/libonnxruntime.so")
            break()
        endif()
    endforeach()
endif()
# ONNX Runtime headers are vendored locally in third_party / provided locally by hints.
if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    message(STATUS "ONNX Runtime found: ${ONNXRUNTIME_LIBRARY}")
    include_directories(${ONNXRUNTIME_INCLUDE_DIR})
    add_definitions(-DHAVE_ONNXRUNTIME)
    
    # Model download: run only when models/clip-image-vitb32.onnx is missing (no re-download if it exists)
    file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/models)
    set(CLIP_VITB32_MODEL "${CMAKE_SOURCE_DIR}/models/clip-image-vitb32.onnx")
    add_custom_command(
        OUTPUT ${CLIP_VITB32_MODEL}
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/scripts/download_placeholder_models.sh
        COMMENT "Ensuring ONNX models exist (download or placeholder; skipped if model already present)"
        VERBATIM
    )
    add_custom_target(download_models DEPENDS ${CLIP_VITB32_MODEL})
    message(STATUS "ONNX models will be downloaded automatically during build (only when missing)")
else()
    message(STATUS "ONNX Runtime not found - QUALITY pipeline will remain stubbed")
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ImageMagick++_INCLUDE_DIRS}
)

# Link directories
link_directories(${ImageMagick++_LIBRARY_DIRS})

# Add subdirectories
add_subdirectory(src)
add_subdirectory(tests)
# Note: examples are now in tests/examples and built via tests/CMakeLists.txt

# Create unified config library
add_library(unified_config_lib STATIC
    src/config/unified_observable_config.cpp
    src/config/config_property_manager.cpp
    src/config/config_property.cpp
    src/config/config_yaml_serializer.cpp
    src/config/config_file_manager.cpp
    src/config/config_file_monitor.cpp
    src/config/config_event_manager.cpp
    src/config/config_change_event.cpp
    src/config/config_validator.cpp
    src/config/config_type_converter.cpp
    src/config/config_manager_factory.cpp
)

target_include_directories(unified_config_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(unified_config_lib
    yaml-cpp::yaml-cpp
)

# Main executable
add_executable(media_dedup_server
    src/main.cpp
    src/core/server.cpp
    src/core/server_logging_setup.cpp
    src/core/server_instance_checker.cpp
    src/core/server_config_manager.cpp
    src/core/server_server_initializer.cpp
    src/core/server_server_shutdown.cpp
    src/core/webserver/web_server_core.cpp
    src/core/webserver/web_handlers_config.cpp
    src/core/webserver/web_handlers_user_settings.cpp
    src/core/webserver/web_handlers_server_status.cpp
    src/core/webserver/web_handlers_websocket.cpp
    src/core/webserver/web_handlers_reset_errors.cpp
    src/core/webserver/web_handlers_thumbnail.cpp
    src/core/webserver/web_handlers_preview.cpp
    src/core/webserver/web_handlers_duplicates.cpp
    src/services/eject_service.cpp
    src/orchestration/integrity_check_service.cpp
    src/core/webserver/web_handlers_cache.cpp
    src/core/server_static_file_handler.cpp
    src/database/database_service.cpp
    src/database/session/session_manager.cpp
    src/database/user_settings/user_settings_ops.cpp
    src/database/scanned_files/scanned_files_ops.cpp
    src/database/processing_errors_ops.cpp
    src/utils/stderr_capture.cpp
    src/utils/thumbnail_generator.cpp
    src/utils/subprocess_runner.cpp
    src/utils/temp_paths.cpp
    src/utils/tool_paths.cpp
    src/core/media_ready_bus.cpp
    src/core/media_generation_failures.cpp
    src/core/media_backfill.cpp
    src/core/media_backfill_hook.cpp
    src/orchestration/thread_pool_manager.cpp
    src/orchestration/scheduler_service.cpp
    src/orchestration/files_manager.cpp
    src/orchestration/duplicate_finder.cpp
    src/filesmanager/file_scanner.cpp
    src/filesmanager/files_service.cpp
    src/filesmanager/media_availability.cpp
    src/filesmanager/disk_cache.cpp
    include/database/user_settings_service.hpp
    src/core/server_console_input_manager.cpp
    src/database/database_manager.cpp
    src/media_processors/media_processor.cpp
    src/media_processors/image_processor.cpp
    src/media_processors/image/image_task.cpp
    src/media_processors/image/pipeline_thumbnail_helper.cpp
    src/media_processors/thumbnail_service.cpp
    src/media_processors/image/backends/image_magick_adapter.cpp
    src/media_processors/image/backends/image_magick_transcoder.cpp
    src/media_processors/image/backends/raw_file_detector.cpp
    src/media_processors/image/backends/raw_embedded_extractor.cpp
    src/media_processors/image/backends/exif_reader.cpp
    src/media_processors/image/backends/heic_adapter.cpp
    src/media_processors/image/backends/tiff_validator.cpp
    src/media_processors/image/backends/transcoding_pipeline.cpp
    src/media_processors/image/pipelines/quality_pipeline.cpp
    src/media_processors/image/backends/onnx_adapter.cpp
    src/media_processors/image/backends/onnx_session_manager.cpp
    src/database/image_artifacts_ops.cpp
    src/database/vector_index.cpp
    src/database/duplicate_groups_ops.cpp
    src/database/day_groups_ops.cpp
    src/database/thumbnail_cache_ops.cpp
    src/database/preview_cache_ops.cpp
    src/database/ejected_files_ops.cpp
    src/media_processors/similarity/similarity_calculator.cpp
    src/media_processors/audio_processor.cpp
    src/media_processors/video_processor.cpp
    src/core/grpc_server/grpc_server_core.cpp
    ${PROTO_BUILD_DIR}/dedup.pb.cc
    ${PROTO_BUILD_DIR}/dedup.grpc.pb.cc
)

# Add model download dependency if ONNX Runtime is available
# Run download_models when building the server so models/ exists and is populated (real or placeholder)
if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    add_dependencies(media_dedup_server download_models)
endif()

# Link libraries
target_link_libraries(media_dedup_server
    Poco::Foundation
    Poco::Data
    Poco::DataSQLite
    Poco::Util
    Poco::Net
    Poco::JSON
    ${SQLITE3_LIBRARIES}
    unified_config_lib
    yaml-cpp::yaml-cpp
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${OpenCV_LIBS}
    ${ONNXRUNTIME_LIBRARY}
    ${ImageMagick++_LIBRARIES}
    ${LIBRAW_LIBRARIES}
    ${LIBTIFF_LIBRARIES}
)

# Add include directories and link directories for LibRaw, libtiff, and SQLite
target_include_directories(media_dedup_server PRIVATE
    ${LIBRAW_INCLUDE_DIRS}
    ${LIBTIFF_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
)
target_link_directories(media_dedup_server PRIVATE
    ${LIBRAW_LIBRARY_DIRS}
    ${LIBTIFF_LIBRARY_DIRS}
    ${GRPC_LIBRARY_DIRS}
    ${PROTOBUF_LIBRARY_DIRS}
)

# Set compile definitions (SQLITE_ENABLE_LOAD_EXTENSION for vector_index / sqlite-vec)
target_compile_definitions(media_dedup_server PRIVATE
    SQLITE_ENABLE_LOAD_EXTENSION
    ${SQLITE3_CFLAGS_OTHER}
)


# Install rules
install(TARGETS media_dedup_server
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# Install configuration files
install(FILES config/config.yaml
    DESTINATION etc/media_dedup_server
)
