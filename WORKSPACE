workspace(name = "hunterclaw")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# ============================================================================
# Bazel Skylib (utilities)
# ============================================================================
http_archive(
    name = "bazel_skylib",
    urls = [
        "https://github.com/bazelbuild/bazel-skylib/releases/download/1.5.0/bazel-skylib-1.5.0.tar.gz",
    ],
    sha256 = "cd55a062e763b9349921f0f5db8c3933288dc8ba4f76dd9416aac68acee3cb94",
)

# ============================================================================
# Using built-in C++ rules
# ============================================================================
# Bazel 3.7.2 uses built-in C++ rules

# ============================================================================
# GoogleTest
# ============================================================================
http_archive(
    name = "com_google_googletest",
    urls = ["https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz"],
    strip_prefix = "googletest-1.14.0",
    sha256 = "8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7",
)

# ============================================================================
# Abseil (required by some headers)
# ============================================================================
http_archive(
    name = "com_google_absl",
    urls = ["https://github.com/abseil/abseil-cpp/archive/refs/tags/20211102.0.tar.gz"],
    strip_prefix = "abseil-cpp-20211102.0",
    sha256 = "dcf71b9cba8dc0ca9940c4b316a0c796be8fab42b070bb6b7cab62b48f0e66c4",
)

# ============================================================================
# Boost (using system boost)
# ============================================================================
# Using system-installed boost libraries via linkopts

# ============================================================================
# Local Dependencies (in external/ directory)
# ============================================================================

# Note: nlohmann/json and yaml-cpp are already in external/ directory
# We'll reference them using local_repository or directly in BUILD files

new_local_repository(
    name = "nlohmann_json",
    path = "external/json",
    build_file = "//third_party:json.BUILD",
)

new_local_repository(
    name = "yaml_cpp",
    path = "external/yaml-cpp",
    build_file = "//third_party:yaml-cpp.BUILD",
)

# ============================================================================
# Rules License (required by BoringSSL)
# ============================================================================
http_archive(
    name = "rules_license",
    urls = [
        "https://github.com/bazelbuild/rules_license/releases/download/0.0.7/rules_license-0.0.7.tar.gz",
    ],
    sha256 = "4531deccb913639c30e5c7512a054d5d875698daeb75d8cf90f284375fe7c360",
)

# ============================================================================
# BoringSSL (Google's OpenSSL fork with Ed25519 support)
# Using an older commit compatible with Bazel 3.7.2
# ============================================================================
http_archive(
    name = "boringssl",
    # Use commit from 2021 that works with older Bazel
    urls = ["https://github.com/google/boringssl/archive/afc30d43eef92979b05776ec0963c9cede5fb80f.tar.gz"],
    strip_prefix = "boringssl-afc30d43eef92979b05776ec0963c9cede5fb80f",
    sha256 = "2b18e1c1ad15cc180529ababde8a62885ac35005131e9a797cdaf0e07d76a767",
)

# ============================================================================
# System Libraries (OpenSSL, libcurl, etc.)
# ============================================================================
# These will be referenced via linkopts in BUILD files
# Example: linkopts = ["-lssl", "-lcrypto", "-lcurl", "-ldl", "-luuid"]
