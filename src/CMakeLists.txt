# Source directory CMakeLists.txt
# This file organizes the source code structure

# Core components
set(CORE_SOURCES
    core/server.cpp
    core/logging_setup.cpp
    core/instance_checker.cpp
    core/config_manager.cpp
    core/server_initializer.cpp
    core/server_shutdown.cpp
)

# Configuration management (legacy ConfigManager removed)
set(CONFIG_SOURCES)

# Database management
set(DATABASE_SOURCES
    database/database_manager.cpp
)

# Combine all sources
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${CONFIG_SOURCES}
    ${DATABASE_SOURCES}
)

# Create source groups for better organization in IDEs
source_group("Core" FILES ${CORE_SOURCES})
source_group("Configuration" FILES ${CONFIG_SOURCES})
source_group("Database" FILES ${DATABASE_SOURCES})
