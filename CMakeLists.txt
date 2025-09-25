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

# Find SQLite3
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# Find yaml-cpp
find_package(yaml-cpp REQUIRED)
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
if(ONNXRUNTIME_LIBRARY AND NOT ONNXRUNTIME_INCLUDE_DIR)
    include(FetchContent)
    if(NOT ONNXRUNTIME_GIT_TAG)
        set(ONNXRUNTIME_GIT_TAG v1.22.2)
    endif()
    message(STATUS "Fetching ONNX Runtime headers from release tarball (tag: ${ONNXRUNTIME_GIT_TAG})")
    FetchContent_Declare(
        onnxruntime_headers
        URL https://github.com/microsoft/onnxruntime/archive/refs/tags/${ONNXRUNTIME_GIT_TAG}.tar.gz
        SOURCE_SUBDIR ""
    )
    FetchContent_MakeAvailable(onnxruntime_headers)
    if(onnxruntime_headers_SOURCE_DIR)
        set(ONNXRUNTIME_INCLUDE_DIR "${onnxruntime_headers_SOURCE_DIR}/include")
    endif()
endif()
if(ONNXRUNTIME_LIBRARY AND ONNXRUNTIME_INCLUDE_DIR)
    if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime/core/session/onnxruntime_cxx_api.h")
        include(FetchContent)
        if(NOT ONNXRUNTIME_GIT_TAG)
            set(ONNXRUNTIME_GIT_TAG v1.22.2)
        endif()
        message(STATUS "Vendoring ONNX Runtime headers from tarball (tag: ${ONNXRUNTIME_GIT_TAG}) due to missing header in provided include dir")
        FetchContent_Declare(
            onnxruntime_headers_fallback
            URL https://github.com/microsoft/onnxruntime/archive/refs/tags/${ONNXRUNTIME_GIT_TAG}.tar.gz
            SOURCE_SUBDIR ""
        )
        FetchContent_MakeAvailable(onnxruntime_headers_fallback)
        if(onnxruntime_headers_fallback_SOURCE_DIR)
            set(ONNXRUNTIME_INCLUDE_DIR "${onnxruntime_headers_fallback_SOURCE_DIR}/include")
        endif()
    endif()
endif()
if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    message(STATUS "ONNX Runtime found: ${ONNXRUNTIME_LIBRARY}")
    include_directories(${ONNXRUNTIME_INCLUDE_DIR})
    add_definitions(-DHAVE_ONNXRUNTIME)
else()
    message(STATUS "ONNX Runtime not found - QUALITY pipeline will remain stubbed")
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add subdirectories
add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(examples)

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
    src/core/server_static_file_handler.cpp
    src/database/database_service.cpp
    src/database/session/session_manager.cpp
    src/database/session/session_lease.cpp
    src/database/user_settings/user_settings_ops.cpp
    src/database/scanned_files/scanned_files_ops.cpp
    src/orchestration/thread_pool_manager.cpp
    src/orchestration/scheduler_service.cpp
    src/orchestration/files_manager.cpp
    src/filesmanager/file_scanner.cpp
    src/filesmanager/files_service.cpp
    include/database/user_settings_service.hpp
    src/core/server_console_input_manager.cpp
    src/database/database_manager.cpp
    src/media_processors/media_processor.cpp
    src/media_processors/image_processor.cpp
    src/media_processors/image/image_task.cpp
    src/media_processors/image/pipelines/fast_pipeline.cpp
    src/media_processors/image/backends/opencv_adapter.cpp
    src/media_processors/image/pipelines/balanced_pipeline.cpp
    src/media_processors/image/pipelines/quality_pipeline.cpp
    src/media_processors/image/backends/features_adapter.cpp
    src/media_processors/image/backends/onnx_adapter.cpp
    src/database/image_artifacts_ops.cpp
    src/media_processors/audio_processor.cpp
    src/media_processors/video_processor.cpp
)

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
    ${OpenCV_LIBS}
    ${ONNXRUNTIME_LIBRARY}
)

# Set compile definitions
target_compile_definitions(media_dedup_server PRIVATE
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
