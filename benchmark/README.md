# Envoy Load Balancer Benchmark

A containerized benchmark tool for comparing Envoy load balancing algorithms using nginx upstream servers.

## Overview

This benchmark tool provides a comprehensive way to test and compare Envoy load balancing algorithms by:

- **Realistic Environment**: 9 fast servers (5ms delay) and 1 slow server (100ms delay)
- **Single nginx Process**: Uses one nginx instance with multiple server blocks for efficiency
- **Comprehensive Metrics**: Reports latency percentiles (P50, P75, P90, P95, P99) and request distribution
- **Containerized**: Docker-based for portability and consistent environment

## Architecture

- **nginx**: Single process with lua modules for delay simulation
- **Envoy**: Load balancer proxy routing requests to nginx servers
- **Benchmark Script**: Python tool that sends requests and analyzes results

## Prerequisites

- Docker
- Built Envoy binary (with contrib extensions for peak_ewma support)

## Building

```bash
cd /srv/apps/envoy/benchmark
docker build -t envoy-benchmark .
```

## Usage

### Basic Usage

```bash
# Compare least_request and peak_ewma algorithms
docker run --rm -v /srv/apps/envoy/linux/amd64/build_envoy-contrib_fastbuild/envoy:/app/envoy \
  envoy-benchmark python3 benchmark.py \
  --envoy_binary_path /app/envoy \
  --algorithm least_request,peak_ewma \
  --requests 1000 --warmup 100
```

### Available Algorithms

- `round_robin`: Distributes requests evenly across all servers
- `least_request`: Routes to server with fewest active requests
- `peak_ewma`: Uses exponentially weighted moving average of response times
- `random`: Randomly selects servers

### Command Line Options

```bash
# See all options
docker run --rm envoy-benchmark python3 benchmark.py --help

# Common options:
--algorithm ALGO1,ALGO2    # Comma-separated list of algorithms to test
--requests N               # Number of benchmark requests (default: 2000)
--warmup N                 # Number of warmup requests (default: 200)
--envoy_binary_path PATH   # Path to Envoy binary
--peak_ewma_decay_time T   # Peak EWMA decay time (default: 10s)
```

## Expected Results

- **Round Robin**: ~10% requests to slow server (1/10 servers)
- **Least Request**: Should favor fast servers but may still hit slow server
- **Peak EWMA**: Should strongly avoid slow server (~1-5% traffic)
- **Random**: ~10% requests to slow server (random distribution)

## Output

The benchmark provides:

1. **Per-algorithm reports**: Request distribution and latency analysis
2. **Summary table**: Comparative latency metrics across algorithms
3. **Analysis**: Whether each algorithm effectively avoided the slow server

## Server Configuration

- **Fast Servers**: 9 servers with 5ms processing delay (ports 19000-19008)
- **Slow Server**: 1 server with 100ms processing delay (port 19009)
- **Envoy Proxy**: Listens on port 8081
- **Admin Interface**: Available on port 9902

## Files

- `benchmark.py`: Main benchmark script
- `requirements.txt`: Python dependencies
- `Dockerfile`: Container configuration
- `README.md`: This documentation

## Troubleshooting

- Ensure Envoy binary is built with contrib extensions for peak_ewma support
- Check that no other processes are using ports 19000-19009, 8081, or 9902
- Verify Docker can access the mounted Envoy binary path