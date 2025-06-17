# FILE: contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/BUILD

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_contrib_extension",
    "envoy_cc_library",
    "envoy_contrib_package",
)

licenses(["notice"])  # Apache 2

envoy_contrib_package()

envoy_cc_library(
    name = "peak_ewma_lb_lib",
    srcs = [
        "config.cc",
        "peak_ewma_lb.cc",
        "peak_ewma_filter.cc",
        "peak_ewma_filter_config.cc",
    ],
    hdrs = [
        "config.h",
        "ewma.h",
        "peak_ewma_lb.h",
        "peak_ewma_filter.h",
        "peak_ewma_filter_config.h",
    ],
    repository = "@envoy",
    deps = [
        "//source/common/common:utility_lib",
        "//source/common/config:utility_lib",
        "//source/common/runtime:runtime_lib",
        "//source/common/upstream:upstream_lib",
        # Corrected dependency from :factory_base_lib to :factory_base
        "//source/extensions/load_balancing_policies/common:factory_base",
        "//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha:pkg_cc_proto",
    ],
)

envoy_cc_contrib_extension(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    deps = [
        ":peak_ewma_lb_lib",
    ],
)
