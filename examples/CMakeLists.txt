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

# Config factory demo
add_executable(config_factory_demo config_factory_demo.cpp)

target_include_directories(config_factory_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(config_factory_demo
    unified_config_lib
    pthread
)



# Install the demos
install(TARGETS unified_config_demo config_factory_demo
    RUNTIME DESTINATION bin
)
