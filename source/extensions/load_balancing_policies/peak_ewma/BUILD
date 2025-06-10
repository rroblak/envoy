# In source/extensions/load_balancing_policies/peak_ewma/BUILD

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_contrib_extension",
    "envoy_cc_library",
    "envoy_contrib_package",
)

licenses(["notice"])  # Apache 2

envoy_contrib_package()

# This library contains all the logic for both the LB policy and its helper filter.
envoy_cc_library(
    name = "peak_ewma_lib",
    srcs = [
        "config.cc",
        "peak_ewma_filter.cc",
        "peak_ewma_filter_config.cc",
        "peak_ewma_lb.cc",
    ],
    hdrs = [
        "config.h",
        "ewma.h",
        "peak_ewma_filter.h",
        "peak_ewma_filter_config.h",
        "peak_ewma_lb.h",
    ],
    repository = "@envoy",
    deps = [
        "//envoy/registry",
        "//envoy/server:filter_config_interface",
        "//source/extensions/filters/http/common:pass_through_filter_lib",
        "//envoy/upstream:load_balancer_interface",
        "//source/common/upstream:upstream_lib",
        "//source/extensions/load_balancing_policies/common:load_balancer_lib",
        "//source/common/runtime:runtime_lib",
        "//envoy/stats:stats_macros",
        "//envoy/common:time_interface",
        # ADDED MISSING DEPENDENCY: This target provides factory_base.h
        "//source/extensions/load_balancing_policies/common:factory_base",
        "@envoy_api//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha:pkg_cc_proto",
    ],
)

# This registers the Load Balancer Policy so it can be used in cluster configurations.
envoy_cc_contrib_extension(
    name = "config_lb",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    repository = "@envoy",
    deps = [
        ":peak_ewma_lib",
    ],
)

# This registers the HTTP filter so it can be used in envoy.yaml filter chains.
envoy_cc_contrib_extension(
    name = "config_filter",
    srcs = ["peak_ewma_filter_config.cc"],
    hdrs = ["peak_ewma_filter_config.h"],
    repository = "@envoy",
    deps = [
        ":peak_ewma_lib",
    ],
)
