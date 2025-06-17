# FILE: contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/test/BUILD

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

# Using the fundamental C++ test rule to avoid complex macro issues.
# This rule is guaranteed to exist in the loaded .bzl file.
envoy_cc_test(
    name = "peak_ewma_lb_test",
    srcs = ["peak_ewma_lb_test.cc"],
    # The repository attribute is important for linking contrib code correctly.
    repository = "@envoy",
    deps = [
        # Link against the implementation library being tested.
        "//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source:peak_ewma_lb_lib",

        # All other necessary dependencies for mocks and utilities.
        # The '//test:main' dependency has been removed, as it is automatically
        # included by the envoy_cc_test rule.
        "//source/common/upstream:upstream_lib",
        "//test/mocks:common_lib",
        "//test/mocks/runtime:runtime_mocks",
        "//test/mocks/upstream:cluster_info_mocks",
        "//test/mocks/upstream:host_mocks",
        "//test/mocks/upstream:priority_set_mocks",
        "//test/test_common:test_runtime_lib",
        "//test/test_common:utility_lib",
        "//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha:pkg_cc_proto",
    ],
)
