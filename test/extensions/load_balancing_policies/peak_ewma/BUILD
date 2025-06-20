load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_test(
    name = "integration_test",
    srcs = ["integration_test.cc"],
    deps = [
        "//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source:peak_ewma_lb_lib",
        "//source/common/common:base64_lib",
        "//source/common/http:utility_lib", 
        "//source/common/protobuf:utility_lib",
        "//test/integration:http_integration_lib",
        "@envoy_api//envoy/config/endpoint/v3:pkg_cc_proto",
    ],
)