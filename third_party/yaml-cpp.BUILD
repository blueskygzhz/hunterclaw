# ============================================================================
# yaml-cpp BUILD file
# ============================================================================

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "yaml-cpp",
    srcs = glob([
        "src/**/*.cpp",
        "src/**/*.h",
    ]),
    hdrs = glob([
        "include/**/*.h",
    ]),
    includes = [
        "include",
        "src",
    ],
    visibility = ["//visibility:public"],
    copts = [
        "-DYAML_CPP_STATIC_DEFINE",
    ],
)
