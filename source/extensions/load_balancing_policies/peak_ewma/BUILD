# In source/extensions/load_balancing_policies/peak_ewma/BUILD

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_contrib_extension", # Use envoy_cc_contrib_extension for contrib
    "envoy_cc_library",
    "envoy_contrib_package",     # Use envoy_contrib_package for contrib
)

licenses(["notice"])  # Apache 2

envoy_contrib_package()

envoy_cc_library(
    name = "peak_ewma_lb_lib",
    srcs = [
        "peak_ewma_lb.cc",
        "config.cc",
        # Add "ewma.cc" here if it exists
    ],
    hdrs = [
        "peak_ewma_lb.h",
        "config.h",
        "ewma.h", # Assuming your local ewma.h
    ],
    repository = "@envoy",
    deps = [
        # Interfaces like Host, ClusterInfo
        "//source/common/upstream:upstream_lib",
        # Runtime utilities (Runtime::Loader)
        "//source/common/runtime:runtime_lib",
        # For stats, including macros
        "//source/common/stats:stats_lib", # Or potentially a more specific stats_macros_lib if it exists
        # For TimeSource, etc.
        "//source/common/common:utility_lib",
        # Protobuf dependency
        "@envoy_api//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha:pkg_cc_proto",
    ],
)

envoy_cc_contrib_extension(
    name = "config",
    # srcs and hdrs for the factory are usually part of the main library
    # unless the factory registration is truly standalone.
    # For simplicity, we assume config.cc (containing the factory) is in peak_ewma_lb_lib.
    # If config.cc only contained the factory registration, it might be listed here.
    # Bazel's envoy_cc_extension expects the registration source.
    # Let's point to config.cc as it will contain the factory and its registration.
    srcs = ["config.cc"],
    hdrs = ["config.h"], # Technically, only config.h is needed if srcs has the impl.
    # status = "alpha", # Temporarily commented out
    # security_posture = "unknown", # Adjust as per security review, Temporarily commented out
    repository = "@envoy",
    deps = [
        ":peak_ewma_lb_lib",
        # For REGISTER_FACTORY and FactoryBase
        "//source/common/config:utility_lib", # Or //source/common/registry:registry_lib
        # For Upstream::TypedLoadBalancerFactory or similar base factory
        "//source/common/upstream:upstream_lib",
        "@envoy_api//contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha:pkg_cc_proto",
    ],
)
