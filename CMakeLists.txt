cmake_minimum_required(VERSION 3.10)
project(dedup_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add Homebrew spdlog path for macOS
set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}:/opt/homebrew/opt/spdlog:/opt/homebrew/opt/yaml-cpp")

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(yaml-cpp REQUIRED)

# Find TBB (Intel Threading Building Blocks)
find_package(TBB REQUIRED)

# Find SQLite3
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

# TODO: DEDUP MODES - Libraries for different deduplication modes
# FAST MODE: OpenCV (dHash) + FFmpeg
# BALANCED MODE: libvips + OpenCV (pHash) + FFmpeg  
# QUALITY MODE: ONNX Runtime + CNN Embeddings (ResNet/CLIP) + FFmpeg

# OpenCV - Required for FAST and BALANCED modes
find_package(PkgConfig REQUIRED)
pkg_check_modules(OpenCV REQUIRED IMPORTED_TARGET opencv4)

# FFmpeg - Required for video processing in all modes
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
)

# LibRaw - Required for raw camera format support
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBRAW REQUIRED IMPORTED_TARGET libraw)

# Individual FFmpeg libraries for linking
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)

# FFmpeg library names - use pkg-config output
execute_process(
    COMMAND pkg-config --libs libavcodec libavformat libavutil libswscale
    OUTPUT_VARIABLE FFMPEG_LIBRARIES
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# libvips - Required for BALANCED mode (image processing)
# TODO: Install with: brew install vips (macOS) or apt-get install libvips-dev (Ubuntu)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS IMPORTED_TARGET vips)

# ONNX Runtime - Required for QUALITY mode (CNN embeddings)
# TODO: Install with: brew install onnxruntime (macOS) or download from ONNX website
find_package(PkgConfig REQUIRED)
pkg_check_modules(ONNXRUNTIME IMPORTED_TARGET libonnxruntime)

# Add include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/core
    ${CMAKE_SOURCE_DIR}/include/auth
    ${CMAKE_SOURCE_DIR}/include/web
    ${CMAKE_SOURCE_DIR}/include/logging
    ${CMAKE_SOURCE_DIR}/include/third_party
    ${OPENSSL_INCLUDE_DIR}
    ${nlohmann_json_INCLUDE_DIRS}
    /opt/homebrew/include
)

# Add Homebrew yaml-cpp lib directory for linker
link_directories(/opt/homebrew/opt/yaml-cpp/lib)

# Add source files
set(SOURCES
    src/main.cpp
    src/auth.cpp
    src/file_utils.cpp
    src/server_config_manager.cpp
    src/media_processor.cpp
    src/database/database_manager.cpp
    src/file_processor.cpp
    src/media_processing_orchestrator.cpp
    src/thread_pool_manager.cpp
    src/database/database_access_queue.cpp
    src/simple_scheduler.cpp
    src/file_scanner.cpp
    src/mount_manager.cpp
    src/singleton_manager.cpp
    src/duplicate_linker.cpp
    src/cache/decoder_cache.cpp
    src/decoder/media_decoder.cpp
    src/transcoding_manager.cpp
)

# Add header files
set(HEADERS
    include/auth/auth.hpp
    include/auth/auth_middleware.hpp
    include/core/status.hpp
    include/core/file_utils.hpp
    include/core/singleton_manager.hpp
    include/core/dedup_modes.hpp
    include/core/server_config_manager.hpp
    include/core/media_processor.hpp
    include/core/simple_scheduler.hpp
    include/core/file_scanner.hpp
    include/database/database_manager.hpp
    include/database/database_access_queue.hpp
    include/core/file_processor.hpp
    include/core/media_processing_orchestrator.hpp
    include/core/thread_pool_manager.hpp
    include/core/transcoding_manager.hpp
    include/logging/logger.hpp
    include/web/openapi_docs.hpp
    include/web/route_handlers.hpp
    include/core/server_config.hpp
    include/core/mount_manager.hpp
    include/core/cache/decoder_cache.hpp
    include/core/decoder/media_decoder.hpp
)

# Create executable
add_executable(dedup_server ${SOURCES} ${HEADERS})

# Link libraries
target_link_libraries(dedup_server
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    pthread
    PkgConfig::SQLITE3
    TBB::tbb
    # TODO: DEDUP MODES - Link libraries for different modes
    # FAST MODE: OpenCV + FFmpeg
    # BALANCED MODE: libvips + OpenCV + FFmpeg
    # QUALITY MODE: ONNX Runtime + FFmpeg
    PkgConfig::OpenCV
    ${FFMPEG_LIBRARIES}
    $<$<BOOL:${VIPS_FOUND}>:PkgConfig::VIPS>
    $<$<BOOL:${ONNXRUNTIME_FOUND}>:PkgConfig::ONNXRUNTIME>
    PkgConfig::LIBRAW
    yaml-cpp
    # macOS native frameworks for faster file enumeration (using C APIs, no frameworks needed)
)

# Add Google Test
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Add httplib
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.14.1
)
FetchContent_MakeAvailable(httplib)

# Add spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
)
FetchContent_MakeAvailable(spdlog)

# Tests
enable_testing()
add_subdirectory(tests)

# Add MediaProcessor example
add_executable(media_processor_example examples/media_processor_example.cpp src/media_processor.cpp src/server_config_manager.cpp src/mount_manager.cpp)
target_include_directories(media_processor_example PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(media_processor_example
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    pthread
    PkgConfig::SQLITE3
    TBB::tbb
    PkgConfig::OpenCV
    ${FFMPEG_LIBRARIES}
    $<$<BOOL:${VIPS_FOUND}>:PkgConfig::VIPS>
    $<$<BOOL:${ONNXRUNTIME_FOUND}>:PkgConfig::ONNXRUNTIME>
    yaml-cpp
)

# Add FileProcessor example
add_executable(file_processor_example examples/file_processor_example.cpp 
               src/media_processor.cpp src/server_config_manager.cpp 
               src/database/database_manager.cpp src/duplicate_linker.cpp src/file_processor.cpp src/file_utils.cpp
               src/database/database_access_queue.cpp src/mount_manager.cpp)
target_include_directories(file_processor_example PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(file_processor_example
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    pthread
    PkgConfig::SQLITE3
    TBB::tbb
    PkgConfig::OpenCV
    ${FFMPEG_LIBRARIES}
    $<$<BOOL:${VIPS_FOUND}>:PkgConfig::VIPS>
    $<$<BOOL:${ONNXRUNTIME_FOUND}>:PkgConfig::ONNXRUNTIME>
    yaml-cpp
)

# Add Raw File Test example
add_executable(raw_file_test examples/raw_file_test.cpp 
               src/media_processor.cpp src/transcoding_manager.cpp 
               src/server_config_manager.cpp src/mount_manager.cpp
               src/database/database_manager.cpp src/duplicate_linker.cpp src/database/database_access_queue.cpp
               src/file_utils.cpp)
target_include_directories(raw_file_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(raw_file_test
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    pthread
    PkgConfig::SQLITE3
    TBB::tbb
    PkgConfig::OpenCV
    ${FFMPEG_LIBRARIES}
    $<$<BOOL:${VIPS_FOUND}>:PkgConfig::VIPS>
    $<$<BOOL:${ONNXRUNTIME_FOUND}>:PkgConfig::ONNXRUNTIME>
    PkgConfig::LIBRAW
    yaml-cpp
)

# Add Cache Size Test example
add_executable(cache_size_test
    examples/cache_size_test.cpp
    src/transcoding_manager.cpp
    src/media_processor.cpp
    src/server_config_manager.cpp
    src/mount_manager.cpp
    src/database/database_manager.cpp
    src/duplicate_linker.cpp
    src/database/database_access_queue.cpp
    src/file_utils.cpp
)

add_executable(file_type_config_test
    examples/file_type_config_test.cpp
    src/server_config_manager.cpp
)

target_include_directories(cache_size_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(cache_size_test
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    pthread
    PkgConfig::SQLITE3
    TBB::tbb
    PkgConfig::OpenCV
    ${FFMPEG_LIBRARIES}
    $<$<BOOL:${VIPS_FOUND}>:PkgConfig::VIPS>
    $<$<BOOL:${ONNXRUNTIME_FOUND}>:PkgConfig::ONNXRUNTIME>
    PkgConfig::LIBRAW
    yaml-cpp
)

target_include_directories(file_type_config_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(file_type_config_test
    PRIVATE
    spdlog::spdlog
    yaml-cpp
    PkgConfig::SQLITE3
    pthread
)
target_include_directories(file_type_config_test PRIVATE /opt/homebrew/include) 

# Add LibRaw probe tool
add_executable(libraw_probe
    examples/libraw_probe.cpp
)
target_include_directories(libraw_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(libraw_probe
    PRIVATE
    PkgConfig::LIBRAW
)

add_executable(raw_to_jpeg
    examples/raw_to_jpeg.cpp
)
target_include_directories(raw_to_jpeg PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(raw_to_jpeg
    PRIVATE
    PkgConfig::LIBRAW
    PkgConfig::OpenCV
)

 