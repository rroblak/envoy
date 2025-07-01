# Envoy Load Balancer Minimal Benchmark

A fast-running benchmark tool for comparing Envoy load balancing algorithms.

## Overview

This benchmark tool provides a quick way to test and compare Envoy load balancing algorithms by:

- **Simple Environment**: 1 consistently fast server (5ms) and 1 server with occasional latency spikes (100ms).
- **Fast Execution**: Runs a small number of requests to get results quickly.
- **Focused Analysis**: Reports on how well each algorithm avoids the spiky server.

## Prerequisites

- Python 3.7+ with `requests` and `pyyaml` packages
- Envoy binary with `peak_ewma` support.

## Usage

```bash
# Compare least_request and peak_ewma
python3 benchmark_linux.py --algorithm least_request,peak_ewma

# Test a single algorithm
python3 benchmark_linux.py --algorithm peak_ewma

# Customize the number of requests
python3 benchmark_linux.py --algorithm peak_ewma --requests 1000 --warmup 100
```

## Expected Results

- **Round Robin**: Should distribute requests evenly (~50% to each server).
- **Least Request**: Should favor the fast server, but may still send significant traffic to the spiky server.
- **Peak EWMA**: Should strongly favor the fast server and send very little traffic to the spiky server.
