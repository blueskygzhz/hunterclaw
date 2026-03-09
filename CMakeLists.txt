cmake_minimum_required(VERSION 3.20)
project(HunterClaw VERSION 0.1.0 LANGUAGES CXX)

# ============================================================================
# C++ Standard
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Build Type
# ============================================================================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ============================================================================
# Compiler Flags
# ============================================================================
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")

# Enable position independent code (for plugins)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================================
# Dependencies
# ============================================================================

# Find Threads
find_package(Threads REQUIRED)

# Find OpenSSL (optional, for TLS)
find_package(OpenSSL)

# Find libcurl (for HTTP client)
find_package(CURL)

# Find libdl (for dynamic loading)
find_library(DL_LIBRARY dl)

# Find libuuid (for UUID generation)
find_library(UUID_LIBRARY uuid)

# ============================================================================
# Include Directories
# ============================================================================
include_directories(
    ${CMAKE_SOURCE_DIR}/include
)

# ============================================================================
# Source Files
# ============================================================================

# Core library
set(CORE_SOURCES
    src/core/types.cc
    src/core/config.cc
    src/core/utils.cc
)

# Plugin system
set(PLUGIN_SOURCES
    src/plugins/manager.cc
    # src/plugins/loader.cc  # Needs interface fixes
)

# Skill system
set(SKILL_SOURCES
    src/skills/manager.cc
    src/skills/scanner.cc
    src/skills/installer.cc
    src/skills/parser_stub.cc
    # src/skills/parser.cc  # Needs yaml-cpp
)

# Channel system
set(CHANNEL_SOURCES
    src/channels/manager.cc
    src/channels/channel_plugin_stub.cc
    # src/channels/feishu_channel.cc  # Will be built as plugin
)

# Gateway
set(GATEWAY_SOURCES
    src/gateway/gateway.cc
    # src/gateway/protocol.cc  # Needs nlohmann/json
)

# Agent system
set(AGENT_SOURCES
    # src/agents/agent.cc  # Has incomplete implementation, temporarily disabled
    src/agents/react_executor.cc
    src/agents/scope.cc
)

# LLM system
set(LLM_SOURCES
    src/llm/llm_client.cc
    src/llm/openai_provider.cc
)

# Tools system
set(TOOLS_SOURCES
    src/tools/tool_manager.cc
    src/tools/builtin_tools.cc
    src/tools/tool_hooks.cc
    src/tools/tool_loop_detector.cc
    src/tools/tool_policy.cc
    src/tools/tool_result_processor_stub.cc
    # src/tools/tool_result_processor.cc  # Needs significant refactoring
)

# Memory system
set(MEMORY_SOURCES
    src/memory/memory_manager.cc
    src/memory/sqlite_storage.cc
    src/memory/compaction.cc
    src/memory/pruning.cc
)

# Sandbox system
set(SANDBOX_SOURCES
    src/sandbox/docker_sandbox.cc
    src/sandbox/sandbox_stub.cc
)

# MCP system
set(MCP_SOURCES
    src/mcp/mcp_stub.cc
    # src/mcp/mcporter_client.cc  # Needs subprocess.h and nlohmann/json
    # src/mcp/mcp_tool_adapter.cc  # Needs nlohmann/json
)

# Session system
set(SESSION_SOURCES
    # src/sessions/session_manager.cc  # Needs nlohmann/json
)

# Queue system
set(QUEUE_SOURCES
    # src/queue/delivery_queue.cc  # Needs nlohmann/json
)

# React system
set(REACT_SOURCES
    # src/react/react_engine.cc  # Needs tool_manager.h
    # src/react/loop_detection.cc
    # src/react/message_processor.cc  # Needs tool_manager.h
)

# Routing system
set(ROUTING_SOURCES
    src/routing/router.cc
)

# ============================================================================
# Build HunterClaw Core Library
# ============================================================================
add_library(hunterclaw_core SHARED
    ${CORE_SOURCES}
    ${AGENT_SOURCES}
    ${LLM_SOURCES}
    ${TOOLS_SOURCES}
    ${PLUGIN_SOURCES}
    ${SKILL_SOURCES}
    ${CHANNEL_SOURCES}
    ${GATEWAY_SOURCES}
    ${MCP_SOURCES}
    ${SESSION_SOURCES}
    ${QUEUE_SOURCES}
    ${REACT_SOURCES}
    ${ROUTING_SOURCES}
    ${SANDBOX_SOURCES}
    # ${MEMORY_SOURCES}  # Memory system needs more implementation
)

target_link_libraries(hunterclaw_core
    Threads::Threads
    jsoncpp
    ${DL_LIBRARY}
    ${UUID_LIBRARY}
)

# Add boost_system for Boost dependencies
find_package(Boost COMPONENTS system)
if(Boost_FOUND)
    target_link_libraries(hunterclaw_core ${Boost_LIBRARIES})
    target_include_directories(hunterclaw_core PRIVATE ${Boost_INCLUDE_DIRS})
endif()

if(CURL_FOUND)
    target_link_libraries(hunterclaw_core ${CURL_LIBRARIES})
    target_include_directories(hunterclaw_core PRIVATE ${CURL_INCLUDE_DIRS})
endif()

if(OPENSSL_FOUND)
    target_link_libraries(hunterclaw_core ${OPENSSL_LIBRARIES})
    target_include_directories(hunterclaw_core PRIVATE ${OPENSSL_INCLUDE_DIR})
endif()

# ============================================================================
# Build Gateway Server Executable
# ============================================================================
add_executable(hunterclaw-gateway
    src/main.cc
)

target_link_libraries(hunterclaw-gateway
    hunterclaw_core
    Threads::Threads
)

# ============================================================================
# Build Example Plugins
# ============================================================================

# Feishu channel plugin (production version)
add_library(hunterclaw_channel_feishu SHARED
    src/channels/feishu_channel.cc
    src/channels/feishu_stub.cc
)
target_link_libraries(hunterclaw_channel_feishu 
    hunterclaw_core
    ${CURL_LIBRARIES}
)
if(CURL_FOUND)
    target_include_directories(hunterclaw_channel_feishu PRIVATE ${CURL_INCLUDE_DIRS})
endif()

# Telegram channel plugin (example)
# add_library(hunterclaw_channel_telegram SHARED
#     plugins/channels/telegram/telegram_channel.cc
# )
# target_link_libraries(hunterclaw_channel_telegram hunterclaw_core)

# ============================================================================
# Install Targets
# ============================================================================
install(TARGETS hunterclaw_core hunterclaw-gateway
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/hunterclaw
    DESTINATION include
)

# ============================================================================
# Testing
# ============================================================================
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    # add_subdirectory(tests)
endif()

# ============================================================================
# Documentation
# ============================================================================
option(BUILD_DOCS "Build documentation" OFF)
if(BUILD_DOCS)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        add_subdirectory(docs)
    endif()
endif()

# ============================================================================
# Packaging
# ============================================================================
set(CPACK_PACKAGE_NAME "HunterClaw")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "High-performance C++ AI Agent platform")
set(CPACK_PACKAGE_VENDOR "HunterClaw Team")
set(CPACK_GENERATOR "TGZ;DEB;RPM")

include(CPack)

# Feishu Channel Test Example
add_executable(test_feishu_channel
    examples/test_feishu_channel.cc
)
target_link_libraries(test_feishu_channel
    hunterclaw_channel_feishu
    hunterclaw_core
)
