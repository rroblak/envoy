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

envoy_cc_test(
    name = "ewma_test",
    srcs = ["ewma_test.cc"],
    deps = [
        "//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source:peak_ewma_lb_lib",
    ],
)

envoy_cc_test(
    name = "config_test",
    srcs = ["config_test.cc"],
    deps = [
        "//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source:peak_ewma_lb_lib",
        "//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source:config",
        "//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha:pkg_cc_proto",
        "//source/common/protobuf:utility_lib",
        "//test/common/stats:stat_test_utility_lib",
        "//test/mocks/runtime:runtime_mocks",
        "//test/mocks/server:factory_context_mocks",
        "//test/mocks/upstream:upstream_mocks",
        "//test/test_common:utility_lib",
    ],
)
