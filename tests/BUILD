# ============================================================================
# HunterClaw Tests BUILD File
# ============================================================================

package(default_visibility = ["//visibility:public"])

# ============================================================================
# Process Integration Test
# ============================================================================
cc_test(
    name = "test_process_integration",
    size = "medium",
    srcs = ["test_process_integration.cc"],
    deps = [
        "//src:hunterclaw_core",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
    copts = [
        "-std=c++17",
    ],
    linkopts = [
        "-lpthread",
    ],
    timeout = "moderate",
)

# ============================================================================
# Additional Test Targets (to be added)
# ============================================================================

# Example: Unit tests for core components
# cc_test(
#     name = "test_subprocess",
#     srcs = ["test_subprocess.cc"],
#     deps = [
#         "//src:hunterclaw_core",
#         "@com_google_googletest//:gtest_main",
#     ],
# )

# Example: Integration tests
# cc_test(
#     name = "test_gateway",
#     srcs = ["test_gateway.cc"],
#     deps = [
#         "//src:hunterclaw_core",
#         "@com_google_googletest//:gtest_main",
#     ],
# )
