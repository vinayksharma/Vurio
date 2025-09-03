# Examples CMakeLists.txt for Media Deduplication Server

# Find yaml-cpp
find_package(yaml-cpp REQUIRED)

# Unified configuration demo
add_executable(unified_config_demo unified_config_demo.cpp)

target_include_directories(unified_config_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(unified_config_demo
    unified_config_lib
    pthread
)



# Install the demo
install(TARGETS unified_config_demo
    RUNTIME DESTINATION bin
)
