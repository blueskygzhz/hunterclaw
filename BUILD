# ============================================================================
# HunterClaw Root BUILD File
# ============================================================================

package(default_visibility = ["//visibility:public"])

# Export all headers
exports_files(glob(["include/**/*.h"]))

# Aggregate target for all headers
cc_library(
    name = "hunterclaw_headers",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)

# Main executable target
alias(
    name = "hunterclaw-gateway",
    actual = "//src:hunterclaw-gateway",
)

# Core library target
alias(
    name = "hunterclaw_core",
    actual = "//src:hunterclaw_core",
)

# Test targets
# Temporarily disabled until tests/BUILD is created
# alias(
#     name = "test_process_integration",
#     actual = "//tests:test_process_integration",
# )
