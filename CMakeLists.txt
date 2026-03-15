cmake_minimum_required(VERSION 3.20)
project(HunterClaw VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++1y")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Dependencies
# ============================================================================

# spdlog (假设已安装)
find_package(spdlog REQUIRED)

# ============================================================================
# Include Directories
# ============================================================================

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# ============================================================================
# Memory Library
# ============================================================================

add_library(hunterclaw_memory STATIC
    # Core
    src/core/utils.cc
    src/core/types.cc
    
    # Memory
    src/memory/compaction.cc
    src/memory/compaction_manager.cc
    src/memory/compaction_hooks.cc
    src/memory/memory_manager.cc
    src/memory/session_memory_provider.cc
)

target_include_directories(hunterclaw_memory PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hunterclaw_memory PUBLIC
    spdlog::spdlog
)

# ============================================================================
# Test Executable (optional)
# ============================================================================

add_executable(test_memory_compaction
    tests/test_memory_compaction.cc
)

target_link_libraries(test_memory_compaction
    hunterclaw_memory
)

# ============================================================================
# Install
# ============================================================================

install(TARGETS hunterclaw_memory
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/hunterclaw
    DESTINATION include
)
