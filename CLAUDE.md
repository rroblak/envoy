# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System and Commands

Envoy uses Bazel as its primary build system. Here are the essential development commands:

### Build Commands
- `bazel build envoy` - Build Envoy static binary (fastbuild mode)
- `bazel build -c opt envoy` - Optimized release build  
- `bazel build -c dbg envoy` - Debug build with symbols
- `bazel build envoy --config=sizeopt` - Size-optimized build
- `bazel build //source/exe:envoy-static` - Build static binary explicitly

### Test Commands
- `bazel test //test/...` - Run all tests
- `bazel test //test/common/http:async_client_impl_test` - Run specific test
- `bazel test --test_output=streamed <target>` - Run with verbose output
- `bazel test --test_env=ENVOY_IP_TEST_VERSIONS=v4only //test/...` - IPv4-only tests
- `bazel test -c dbg --config=asan //test/...` - Run with AddressSanitizer
- `bazel test -c dbg --config=msan //test/...` - Run with MemorySanitizer
- `bazel test -c dbg --config=tsan //test/...` - Run with ThreadSanitizer

### Linting and Formatting
- `bazel run //tools/code_format:check_format -- check` - Check code formatting
- `bazel run //tools/code_format:check_format -- fix` - Fix code formatting
- `./tools/spelling/check_spelling_pedantic.py check` - Check spelling
- `./tools/spelling/check_spelling_pedantic.py fix` - Fix spelling

### Development Convenience Scripts

#### build_envoy Function (Recommended)
A convenience shell function is available for optimized Docker builds with persistent caching:

- `build_envoy` - Build and test contrib extensions with cached dependencies (default)
- `build_envoy --peak-ewma` or `build_envoy -p` - Build only Peak EWMA extensions with tests
- `build_envoy dev` - Build standard Envoy without contrib extensions
- `build_envoy coverage` - Generate coverage report
- `build_envoy release.server_only` - Release build

The function automatically uses persistent bazel cache and cached binary to avoid re-downloads.

#### Direct Docker Scripts
- `./ci/run_envoy_docker.sh './ci/do_ci.sh dev'` - Build and test in Docker
- `./ci/run_envoy_docker.sh './ci/do_ci.sh dev.contrib'` - Build with contrib extensions and test in Docker
- `./ci/run_envoy_docker.sh './ci/do_ci.sh release.server_only'` - Release build in Docker
- `./ci/run_envoy_docker.sh './ci/do_ci.sh coverage'` - Generate coverage report in Docker

#### Build Output Locations
After running Docker builds, the Envoy binaries are copied to the host filesystem:

**Standard Envoy Binary:**
- `linux/amd64/build_envoy_fastbuild/envoy` - Fast build
- `linux/amd64/build_envoy_opt/envoy` - Optimized build  
- `linux/amd64/build_envoy_dbg/envoy` - Debug build

**Contrib Envoy Binary (includes Peak EWMA):**
- `linux/amd64/build_envoy-contrib_fastbuild/envoy` - Fast build with contrib extensions
- `linux/amd64/build_envoy-contrib_opt/envoy` - Optimized build with contrib extensions
- `linux/amd64/build_envoy-contrib_dbg/envoy` - Debug build with contrib extensions

**Additional Tools:**
- `config_load_check_tool` - Configuration validation tool
- `router_check_tool` - Route configuration testing tool  
- `schema_validator_tool` - Schema validation utility

### Coverage and Analysis
- `./ci/run_envoy_docker.sh './ci/do_ci.sh coverage'` - Generate coverage report in Docker (recommended)
- `./ci/run_envoy_docker.sh './ci/do_ci.sh coverage //contrib/...'` - Generate coverage report for contrib extensions
- `test/run_envoy_bazel_coverage.sh` - Generate coverage report (direct Bazel)
- `tools/gen_compilation_database.py` - Generate compilation database for IDEs

### Troubleshooting

#### Bazel Download 403 Error
If you encounter a 403 error when bazelisk tries to download Bazel (e.g., "HTTP GET https://releases.bazel.build/7.6.0/release/bazel-7.6.0-linux-x86_64 failed with error 403"), follow these steps:

1. **Download Bazel manually to persistent storage:**
   ```bash
   mkdir -p /mnt/persistent/bazel-cache
   wget -O /mnt/persistent/bazel-cache/bazel-7.6.0-linux-x86_64 https://github.com/bazelbuild/bazel/releases/download/7.6.0/bazel-7.6.0-linux-x86_64
   ```

2. **Create bazelisk cache structure on persistent storage:**
   ```bash
   mkdir -p /mnt/persistent/bazel-cache/.cache/bazelisk/downloads/sha256
   mkdir -p /mnt/persistent/bazel-cache/.cache/bazelisk/bin
   ```

3. **Copy to cache with SHA256 name:**
   ```bash
   SHA256=$(sha256sum /mnt/persistent/bazel-cache/bazel-7.6.0-linux-x86_64 | cut -d' ' -f1)
   cp /mnt/persistent/bazel-cache/bazel-7.6.0-linux-x86_64 /mnt/persistent/bazel-cache/.cache/bazelisk/downloads/sha256/$SHA256
   cp /mnt/persistent/bazel-cache/bazel-7.6.0-linux-x86_64 /mnt/persistent/bazel-cache/.cache/bazelisk/bin/bazel-7.6.0-linux-x86_64
   chmod +x /mnt/persistent/bazel-cache/.cache/bazelisk/bin/bazel-7.6.0-linux-x86_64
   ```

4. **Use cached binary and persistent build cache in Docker builds:**
   ```bash
   ENVOY_DOCKER_BUILD_DIR="/mnt/persistent/bazel-cache/docker-build" \
   BAZEL_STARTUP_OPTIONS="--output_user_root=/build/bazel-cache --output_base=/build/bazel-cache/base" \
   ENVOY_DOCKER_OPTIONS="-v /mnt/persistent/bazel-cache/.cache/bazelisk:/build/.cache/bazelisk -v /mnt/persistent/bazel-cache:/build/bazel-cache -e USE_BAZEL_VERSION=/build/.cache/bazelisk/bin/bazel-7.6.0-linux-x86_64" \
   ./ci/run_envoy_docker.sh './ci/do_ci.sh dev.contrib'
   ```

This bypasses the bazelisk download and ensures the entire build cache (Bazel binary, build artifacts, and dependencies) persists on the /mnt/persistent volume between builds, avoiding re-downloads and significantly speeding up subsequent builds.

## Repository Architecture

### Core Directories
- **`source/`** - All Envoy source code
  - **`source/common/`** - Core Envoy functionality (not extension-specific)
  - **`source/exe/`** - Main binary and platform-specific entry points
  - **`source/server/`** - Server implementation (config, workers, lifecycle)
  - **`source/extensions/`** - All extension implementations
- **`envoy/`** - Public interface headers (mostly abstract classes)
- **`test/`** - All test code matching source structure
- **`api/`** - Envoy data plane API definitions
- **`bazel/`** - Bazel build configuration and rules
- **`contrib/`** - Contrib extensions (less stable/supported)

### Extension Structure
Extensions follow a strict namespace and directory layout:
- Access loggers: `source/extensions/access_loggers/` → `Envoy::Extensions::AccessLoggers`
- HTTP filters: `source/extensions/filters/http/` → `Envoy::Extensions::HttpFilters`
- Network filters: `source/extensions/filters/network/` → `Envoy::Extensions::NetworkFilters`
- Transport sockets: `source/extensions/transport_sockets/` → `Envoy::Extensions::TransportSockets`
- Each extension lives in its own namespace

### Key Architectural Components
- **Connection Manager** - Core HTTP processing (`source/common/http/conn_manager_impl.*`)
- **Filter Chain** - Request/response processing pipeline
- **Upstream Management** - Load balancing and health checking (`source/common/upstream/`)
- **Router** - Request routing logic (`source/common/router/`)
- **Network Layer** - Socket and connection management (`source/common/network/`)
- **TLS/SSL** - Secure transport implementation (`source/common/tls/`)

## Development Guidelines

### Code Organization
- Extensions should be placed in appropriate subdirectories under `source/extensions/`
- Common code shared by multiple extensions goes in `common/` directories
- Follow existing naming conventions and namespace patterns
- Use the established header include patterns

### Testing Requirements
- Unit tests must be in `test/` directories matching source structure
- Integration tests go in `test/integration/`
- All new code requires 100% test coverage
- Use existing mock implementations in `test/mocks/`

### Build Configuration
- Extensions are registered in `source/extensions/extensions_build_config.bzl`
- Extensions can be disabled with `--//source/extensions/path:enabled=false`
- Custom extension configurations can be created by overriding `@envoy_build_config`

### Compiler and Toolchain
- Default toolchain uses Clang with libc++ (`--config=clang`)
- GCC with libstdc++ also supported (`--config=gcc`)
- Remote execution available with `--config=remote-clang`
- Docker sandbox builds with `--config=docker-clang`

### Style and Standards
- Code must pass format checking (`bazel run //tools/code_format:check_format`)
- Follow inclusive language policy (no allowlist/denylist, primary/secondary)
- All code comments and documentation require proper English
- Use runtime guards for high-risk changes

### Common Development Patterns
- Extensions implement factory patterns with typed configurations
- Use proto configuration for all extension settings
- Implement proper filter state management for request/response processing
- Follow established patterns for stats collection and logging

### Dependencies and External Libraries
- External dependencies managed through Bazel WORKSPACE and repositories.bzl
- Security patches and dependency updates follow strict policies
- Check `bazel/EXTERNAL_DEPS.md` for dependency management guidelines

This repository represents the Envoy Proxy project - a high-performance edge/middle/service proxy built in C++. The codebase is mature, well-tested, and follows strict development practices including comprehensive testing, security reviews, and performance optimization.

## Peak EWMA Load Balancer Development Status

### Current Implementation: Phase 4 Complete ✅
The Peak EWMA load balancer implementation is now feature-complete with timer-based aggregation:

**Core Features:**
- **Lock-free RTT Collection**: Worker threads record RTT samples in circular buffers without blocking
- **Automatic Aggregation**: Timer triggers every 100ms (configurable) to collect samples from all threads  
- **Memory Bounded**: Configurable circular buffers (default 25K samples per host per worker)
- **Real-time Load Balancing**: Power of Two Choices algorithm with latency-aware cost function
- **Production Ready**: Full integration with HTTP filter and comprehensive test coverage

**Architecture:**
1. **HTTP Filter** (`contrib/envoy/extensions/filters/http/peak_ewma/`) - Records RTT samples
2. **Load Balancer** (`contrib/envoy/extensions/load_balancing_policies/peak_ewma/`) - P2C selection with EWMA costs
3. **Double Buffer System** - Lock-free sample collection via atomic buffer swapping
4. **Timer Aggregation** - Periodic cross-thread sample collection and EWMA updates

**Performance Characteristics:**
- Supports ~250K RPS per worker with default settings
- ~400KB memory per host per worker at capacity
- 100ms aggregation interval balances freshness vs overhead
- O(1) host selection complexity via P2C algorithm

**Configuration:**
```yaml
load_balancing_policy:
  policies:
  - typed_extension_config:
      name: envoy.load_balancing_policies.peak_ewma
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.load_balancing_policies.peak_ewma.v3alpha.PeakEwma
        decay_time: 10s              # EWMA decay time (default: 10s)
        aggregation_interval: 100ms  # Cross-thread collection interval (default: 100ms)  
        max_samples_per_host: 25000  # Buffer size per host per worker (default: 25K)
```

**Implementation Phases Completed:**
- ✅ Phase 1: Core P2C load balancer with basic EWMA
- ✅ Phase 2: Atomic GlobalHostStats data structure
- ✅ Phase 3: HTTP filter integration for RTT measurement
- ✅ Phase 4: Timer-based aggregation with lock-free circular buffers

**Testing:**
- Unit tests with comprehensive mock infrastructure
- Integration tests with configurable latency simulation
- All tests passing with production-ready timer system

### Race Condition Investigation (July 2025)

**Problem**: Implementation shows inconsistent effectiveness - sometimes perfect slow server avoidance (100% fast servers) and sometimes reduced effectiveness (95.7% fast, 4.3% slow). The algorithm is working but with variable performance, suggesting runtime data races rather than complete initialization failure.

**Benchmark Evidence:**
- "Good" run: 1000/0 requests (fast/slow) - 100% effectiveness
- "Bad" run: 957/43 requests (fast/slow) - 95.7% effectiveness
- Algorithm consistently detects and mostly avoids slow servers, but with varying precision

**Failed Fixes Attempted:**
1. **GlobalHostStats Constructor Race Fix (FAILED)**: 
   - Theory: HTTP filter records RTT samples before `setLoadBalancer()` is called
   - Fix: Pass load balancer pointer in constructor instead of separate `setLoadBalancer()` call
   - Result: No change in behavior - race condition persists
   - Files modified and reverted: `peak_ewma_lb.h`, `peak_ewma_lb.cc`, `peak_ewma_filter_test.cc`

**Current Leading Theory - Runtime Data Races:**
- **Buffer Swap Race in `collectAndSwapBuffers()`**: Worker threads writing to "swapped" buffer during collection
- **Sample Loss**: Inconsistent RTT sample collection leads to variable EWMA accuracy
- **EWMA Convergence Variability**: Missing samples affect convergence speed and precision
- **Cost Calculation Inconsistency**: Occasional use of stale/incomplete EWMA data

**Next Investigation Areas:**
- Fix buffer swap race condition in `PerThreadHostStats::collectAndSwapBuffers()` (peak_ewma_lb.cc:119-131)
- Memory ordering issues in atomic operations
- Thread-local storage timing issues with sample collection