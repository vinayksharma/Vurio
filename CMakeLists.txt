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
    src/core/webserver/server_core.cpp
    src/core/webserver/handlers_config.cpp
    src/core/webserver/handlers_user_settings.cpp
    src/database/database_service.cpp
    src/database/session/session_manager.cpp
    src/database/session/session_lease.cpp
    src/database/user_settings/user_settings_ops.cpp
    src/database/scanned_files/scanned_files_ops.cpp
    src/filesmanager/files_service.cpp
    include/database/user_settings_service.hpp
    src/core/console_input_manager.cpp
    src/database/database_manager.cpp
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
