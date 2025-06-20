# In contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/test/BUILD

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_contrib_package",
)

licenses(["notice"])  # Apache 2

envoy_contrib_package()

envoy_cc_test(
    name = "peak_ewma_lb_test",
    srcs = ["peak_ewma_lb_test.cc"],
    deps = [
        # The library containing the load balancer implementation under test.
        "//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source:peak_ewma_lb_lib",
        
        # Dependencies for mocks and test utilities used in the test file.
        "//source/common/network:address_lib", # For Ipv4Instance
        "//test/common/stats:stat_test_utility_lib",
        "//test/mocks:common_lib",
        "//test/mocks/network:network_mocks",
        "//test/mocks/runtime:runtime_mocks",
        "//test/mocks/upstream:upstream_mocks",
        "//test/test_common:status_utility_lib", # For ASSERT_OK
    ],
)
