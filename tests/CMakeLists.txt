# HunterClaw Tests CMakeLists.txt

cmake_minimum_required(VERSION 3.14)

# 查找 Google Test
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# 查找其他依赖
find_package(Threads REQUIRED)

# 包含项目头文件
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# Gateway & Channel 单元测试
# ============================================================================

add_executable(test_gateway_channel_unit
    test_gateway_channel_unit.cc
)

target_link_libraries(test_gateway_channel_unit
    # Google Test
    ${GTEST_LIBRARIES}
    ${GTEST_MAIN_LIBRARIES}
    
    # 项目库
    hunterclaw_gateway
    hunterclaw_channels
    hunterclaw_core
    
    # 系统库
    Threads::Threads
    pthread
)

# 添加测试
add_test(NAME GatewayChannelUnitTest COMMAND test_gateway_channel_unit)

# ============================================================================
# 测试配置
# ============================================================================

# 启用测试
enable_testing()

# 设置测试超时
set_tests_properties(GatewayChannelUnitTest PROPERTIES TIMEOUT 60)

# 输出测试结果
set_tests_properties(GatewayChannelUnitTest PROPERTIES 
    ENVIRONMENT "GTEST_OUTPUT=xml:test_results.xml"
)

# ============================================================================
# 测试目标
# ============================================================================

# 运行所有测试
add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --verbose
    DEPENDS test_gateway_channel_unit
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# 生成测试覆盖率报告（需要 gcov/lcov）
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_custom_target(coverage
        COMMAND lcov --capture --directory . --output-file coverage.info
        COMMAND lcov --remove coverage.info '/usr/*' --output-file coverage.info
        COMMAND lcov --list coverage.info
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
