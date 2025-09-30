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

# Quality smoke example (optional)
add_executable(quality_smoke quality_smoke.cpp)
target_include_directories(quality_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(quality_smoke
    unified_config_lib
    pthread
    ${OpenCV_LIBS}
    ${ONNXRUNTIME_LIBRARY}
    ${ImageMagick++_LIBRARIES}
    Poco::Foundation
    Poco::Data
    Poco::DataSQLite
)
target_sources(quality_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/src/database/database_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/database/session/session_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/database/session/session_lease.cpp
    ${CMAKE_SOURCE_DIR}/src/database/image_artifacts_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/pipelines/quality_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/onnx_adapter.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/onnx_session_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/image_magick_adapter.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/raw_file_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/media_processors/image/backends/transcoding_pipeline.cpp
)
