# In api/contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/BUILD

load("@envoy_api//bazel:api_build_system.bzl", "api_proto_package")

licenses(["notice"])  # Apache 2

api_proto_package(
    has_services = False,
    deps = [
        "@com_github_cncf_xds//xds/annotations/v3:pkg",  # Provides xds/annotations/v3/status.proto
        "@com_github_cncf_xds//udpa/annotations:pkg", # Needed because xds...status.proto imports udpa...status.proto
        # Note: The core BUILD file (api/envoy/config/core/v3/BUILD) also included:
        # "@envoy_api//envoy/annotations:pkg",
        # "@envoy_api//envoy/type/matcher/v3:pkg",
        # "@envoy_api//envoy/type/v3:pkg",
        # "@com_github_cncf_xds//xds/core/v3:pkg",
        # You can add these back if further errors indicate they are needed for some
        # transitive reason, but let's start with the direct requirements for the status annotations.
    ],
)