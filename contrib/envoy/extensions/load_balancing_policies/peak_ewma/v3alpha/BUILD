# FILE: contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/BUILD

load("@envoy_api//bazel:api_build_system.bzl", "api_proto_package")

licenses(["notice"])  # Apache 2

# This rule compiles the .proto files in this directory into the C++ targets
# (like pkg_cc_proto) that the rest of the build system needs.
api_proto_package(
    has_services = False,
    deps = [
        "@com_github_cncf_xds//xds/annotations/v3:pkg",
        "@com_github_cncf_xds//udpa/annotations:pkg",
    ],
)
