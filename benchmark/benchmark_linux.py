#!/usr/bin/env python3
"""
Envoy Load Balancer Minimal Benchmark Tool

This script benchmarks load balancing algorithms with a simple, fast-running setup:
1. Starts one consistently fast HTTP server.
2. Starts one HTTP server with occasional latency spikes.
3. Configures and runs Envoy with the specified load balancer.
4. Sends a small number of requests to quickly evaluate performance.
5. Reports on request distribution and latency.

Usage:
    python3 benchmark/benchmark_linux.py --algorithm peak_ewma,least_request
"""

import argparse
import json
import time
import statistics
import subprocess
import tempfile
import os
import sys
import signal
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Dict, List, Tuple, Optional
import requests
from http.server import HTTPServer, BaseHTTPRequestHandler
import threading
import numpy as np
from tabulate import tabulate
import shutil
import random
import yaml

class SimpleHTTPHandler(BaseHTTPRequestHandler):
    """HTTP handler for a consistently fast server."""
    def do_GET(self):
        delay_ms = getattr(self.server, 'delay_ms', 5)
        time.sleep(delay_ms / 1000.0)
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        response = json.dumps({"server": getattr(self.server, 'server_name', 'fast'), "delay_ms": delay_ms})
        try:
            self.wfile.write(response.encode())
        except BrokenPipeError:
            pass

    def log_message(self, format, *args):
        pass

class SpikyLatencyHandler(BaseHTTPRequestHandler):
    """HTTP handler with occasional high-latency spikes."""
    def do_GET(self):
        server_name = getattr(self.server, 'server_name', 'spiky')
        base_delay_ms = getattr(self.server, 'base_delay', 5)
        spike_delay_ms = getattr(self.server, 'spike_delay', 100)

        # 90% chance of base delay, 10% chance of a spike
        delay_ms = base_delay_ms if random.random() < 0.9 else spike_delay_ms
        time.sleep(delay_ms / 1000.0)

        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        response = json.dumps({"server": server_name, "delay_ms": delay_ms})
        try:
            self.wfile.write(response.encode())
        except BrokenPipeError:
            pass

    def log_message(self, format, *args):
        pass

class MinimalBenchmark:
    def __init__(self, algorithm: str, num_requests: int = 2000, warmup_requests: int = 200, envoy_binary_path: str = "", peak_ewma_decay_time: str = "10s"):
        self.algorithm = algorithm
        self.num_requests = num_requests
        self.warmup_requests = warmup_requests
        self.peak_ewma_decay_time = peak_ewma_decay_time
        self.fast_delay_ms = 5
        self.slow_delay_ms = 100
        self.num_fast_servers = 9
        self.num_slow_servers = 1
        self.servers = []
        self.server_threads = []
        self.envoy_process = None
        self.server_requests = {}
        self.response_times = []
        self.base_port = 19000
        self.envoy_port = 8081
        self.envoy_admin_port = 9902
        self.envoy_binary_path = envoy_binary_path

    def get_envoy_binary_path(self) -> str:
        if self.envoy_binary_path:
            return os.path.abspath(self.envoy_binary_path)
        # Default to the contrib build binary
        default_path = "/srv/apps/envoy/linux/amd64/build_envoy-contrib_fastbuild/envoy"
        if os.path.exists(default_path):
            return default_path
        raise RuntimeError("Envoy binary path not provided and default contrib binary not found. Please specify with --envoy_binary_path.")

    def start_test_servers(self) -> None:
        print("Starting test servers...")
        
        # Start 9 fast servers
        for i in range(self.num_fast_servers):
            fast_server = HTTPServer(('127.0.0.1', self.base_port + i), SimpleHTTPHandler)
            fast_server.server_name = f'fast_{i}'
            fast_server.delay_ms = self.fast_delay_ms
            fast_thread = threading.Thread(target=fast_server.serve_forever, daemon=True)
            fast_thread.start()
            self.servers.append(fast_server)

        # Start 1 slow server
        slow_server = HTTPServer(('127.0.0.1', self.base_port + self.num_fast_servers), SimpleHTTPHandler)
        slow_server.server_name = 'slow'
        slow_server.delay_ms = self.slow_delay_ms
        slow_thread = threading.Thread(target=slow_server.serve_forever, daemon=True)
        slow_thread.start()
        self.servers.append(slow_server)
        
        time.sleep(1)
        print(f"Test servers started: {self.num_fast_servers} fast, {self.num_slow_servers} slow.")

    def stop_test_servers(self) -> None:
        for server in self.servers:
            server.shutdown()
            server.server_close()

    def create_envoy_config(self, temp_dir: str) -> str:
        endpoints = []
        # Add all servers (9 fast + 1 slow)
        for i in range(self.num_fast_servers + self.num_slow_servers):
            endpoints.append({
                "endpoint": {"address": {"socket_address": {"address": "127.0.0.1", "port_value": self.base_port + i}}}
            })

        policy_map = {
            "round_robin": "envoy.extensions.load_balancing_policies.round_robin.v3.RoundRobin",
            "peak_ewma": "envoy.extensions.load_balancing_policies.peak_ewma.v3alpha.PeakEwma",
            "least_request": "envoy.extensions.load_balancing_policies.least_request.v3.LeastRequest",
            "random": "envoy.extensions.load_balancing_policies.random.v3.Random",
        }
        
        # Configure load balancing policy with Peak EWMA parameters
        typed_config = {"@type": f"type.googleapis.com/{policy_map[self.algorithm]}"}
        if self.algorithm == "peak_ewma":
            typed_config["decay_time"] = self.peak_ewma_decay_time
        
        lb_policy = {
            "policies": [{
                "typed_extension_config": {
                    "name": f"envoy.load_balancing_policies.{self.algorithm}",
                    "typed_config": typed_config
                }
            }]
        }

        http_filters = []
        if self.algorithm == "peak_ewma":
            http_filters.append({"name": "envoy.filters.http.peak_ewma", "typed_config": {"@type": "type.googleapis.com/envoy.extensions.filters.http.peak_ewma.v3alpha.PeakEwmaConfig"}})
        http_filters.append({"name": "envoy.filters.http.router", "typed_config": {"@type": "type.googleapis.com/envoy.extensions.filters.http.router.v3.Router"}})

        envoy_config = {
            "admin": {"address": {"socket_address": {"address": "127.0.0.1", "port_value": self.envoy_admin_port}}},
            "static_resources": {
                "clusters": [{
                    "name": "test_cluster", "type": "STATIC", "load_balancing_policy": lb_policy,
                    "load_assignment": {"cluster_name": "test_cluster", "endpoints": [{"lb_endpoints": endpoints}]}
                }],
                "listeners": [{
                    "name": "listener_0", "address": {"socket_address": {"address": "127.0.0.1", "port_value": self.envoy_port}},
                    "filter_chains": [{"filters": [{"name": "envoy.filters.network.http_connection_manager", "typed_config": {
                        "@type": "type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager",
                        "stat_prefix": "ingress_http", "codec_type": "AUTO",
                        "route_config": {"name": "local_route", "virtual_hosts": [{"name": "local_service", "domains": ["*"],
                            "routes": [{"match": {"prefix": "/"}, "route": {"cluster": "test_cluster"}}]
                        }]},
                        "http_filters": http_filters
                    }}]}],
                }]
            }
        }
        
        config_path = os.path.join(temp_dir, 'envoy.yaml')
        with open(config_path, 'w') as f:
            yaml.dump(envoy_config, f)
        return config_path

    def start_envoy(self, config_path: str) -> None:
        envoy_binary = self.get_envoy_binary_path()
        print(f"Starting Envoy ({self.algorithm}) with binary: {envoy_binary}")
        print(f"Envoy binary path being used: {envoy_binary}") # Added for debugging
        self.envoy_process = subprocess.Popen([envoy_binary, '-c', config_path, '--log-level', 'warn'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(3) # Wait for Envoy to start
        try:
            requests.get(f"http://127.0.0.1:{self.envoy_port}/", timeout=1)
        except requests.exceptions.RequestException:
            stdout, stderr = self.envoy_process.communicate()
            raise RuntimeError(f"Envoy failed to start. Stderr: {stderr.decode()}")

    def stop_envoy(self) -> None:
        if self.envoy_process:
            self.envoy_process.terminate()
            self.envoy_process.wait(timeout=5)

    def send_request(self) -> Optional[Tuple[str, float]]:
        start_time = time.time()
        try:
            response = requests.get(f"http://127.0.0.1:{self.envoy_port}/", timeout=2)
            if response.status_code == 200:
                return response.json()["server"], (time.time() - start_time) * 1000
        except requests.exceptions.RequestException:
            return None
        return None

    def run_phase(self, phase_name: str, num_requests: int):
        print(f"Running {phase_name} with {num_requests} requests...")
        with ThreadPoolExecutor(max_workers=10) as executor:
            futures = [executor.submit(self.send_request) for _ in range(num_requests)]
            for future in as_completed(futures):
                result = future.result()
                if result:
                    server_name, response_time = result
                    if phase_name == "benchmark":
                        self.server_requests.setdefault(server_name, 0)
                        self.server_requests[server_name] += 1
                        self.response_times.append(response_time)

    def print_test_parameters(self):
        print("\n=== BENCHMARK PARAMETERS ===")
        print(f"Clients: 10 concurrent threads (single-threaded HTTP client per thread)")
        print(f"Upstream servers: {self.num_fast_servers + self.num_slow_servers}")
        print(f"  - Fast servers ({self.num_fast_servers}): {self.fast_delay_ms}ms latency")
        print(f"  - Slow servers ({self.num_slow_servers}): {self.slow_delay_ms}ms latency")
        print(f"Requests per algorithm: {self.num_requests} (after {self.warmup_requests} warmup requests)")
        print("\n")

    def generate_report(self) -> dict:
        if not self.response_times: 
            return {"error": "No data collected."}
        
        successful_requests = sum(self.server_requests.values())
        fast_reqs = sum(self.server_requests.get(f'fast_{i}', 0) for i in range(self.num_fast_servers))
        slow_reqs = self.server_requests.get('slow', 0)
        
        # Calculate detailed statistics
        times = np.array(self.response_times)
        stats = {
            "algorithm": self.algorithm,
            "average": float(np.mean(times)),
            "p50": float(np.percentile(times, 50)),
            "p75": float(np.percentile(times, 75)),
            "p90": float(np.percentile(times, 90)),
            "p95": float(np.percentile(times, 95)),
            "p99": float(np.percentile(times, 99)),
            "p999": float(np.percentile(times, 99.9)),
            "min": float(np.min(times)),
            "max": float(np.max(times)),
            "std_dev": float(np.std(times)),
            "fast_requests": fast_reqs,
            "slow_requests": slow_reqs,
            "total_requests": successful_requests
        }
        
        # Generate text report
        report = f"\n--- Report for {self.algorithm.upper()} ---\n"
        report += f"Avg Latency: {stats['average']:.2f}ms\n"
        report += f"P95 Latency: {stats['p95']:.2f}ms\n"
        report += f"Request Distribution:\n"
        report += f"  - Fast Servers: {fast_reqs} requests ({fast_reqs/successful_requests:.1%})\n"
        report += f"  - Slow Server: {slow_reqs} requests ({slow_reqs/successful_requests:.1%})\n"
        
        # Analysis
        slow_traffic_pct = (slow_reqs / successful_requests) * 100
        if self.algorithm in ["least_request", "peak_ewma"]:
            if slow_traffic_pct < 5:  # Should be much less than 10% (1/10 servers)
                report += "ANALYSIS: ✅ Algorithm effectively avoided the slow server.\n"
            else:
                report += "ANALYSIS: ❌ Algorithm did NOT effectively avoid the slow server.\n"
        elif self.algorithm in ["round_robin", "random"]:
             if 8 < slow_traffic_pct < 12:  # Should be around 10% (1/10 servers)
                report += f"ANALYSIS: ✅ {self.algorithm.title()} distributed requests evenly as expected.\n"
             else:
                report += f"ANALYSIS: ❌ {self.algorithm.title()} did NOT distribute requests evenly.\n"
        else:
            # Unknown algorithm, just report the distribution
            report += f"ANALYSIS: Slow server received {slow_traffic_pct:.1f}% of traffic.\n"
        
        stats["report_text"] = report
        return stats

    def run(self) -> dict:
        with tempfile.TemporaryDirectory() as temp_dir:
            try:
                self.start_test_servers()
                config_path = self.create_envoy_config(temp_dir)
                self.start_envoy(config_path)
                self.run_phase("warmup", self.warmup_requests)
                self.run_phase("benchmark", self.num_requests)
                return self.generate_report()
            finally:
                self.stop_envoy()
                self.stop_test_servers()

def main():
    parser = argparse.ArgumentParser(description="Minimal Envoy Load Balancer Benchmark")
    parser.add_argument("--algorithm", "-a", required=True, help="LB algorithm(s) to test (comma-separated)")
    parser.add_argument("--requests", "-r", type=int, default=2000, help="Number of benchmark requests")
    parser.add_argument("--warmup", "-w", type=int, default=200, help="Number of warmup requests")
    parser.add_argument("--envoy_binary_path", type=str, default="", help="Path to the Envoy binary (e.g., bazel-bin/source/exe/envoy-static)")
    parser.add_argument("--peak_ewma_decay_time", type=str, default="10s", help="Peak EWMA decay time (e.g., 5s, 10s, 30s)")
    args = parser.parse_args()

    algorithms = [algo.strip() for algo in args.algorithm.split(',')]
    
    # Print test parameters once at the beginning
    if algorithms:
        benchmark_sample = MinimalBenchmark(algorithms[0], args.requests, args.warmup, args.envoy_binary_path, args.peak_ewma_decay_time)
        benchmark_sample.print_test_parameters()
    
    results = []
    
    for algorithm in algorithms:
        print("="*40)
        print(f"BENCHMARKING: {algorithm.upper()}")
        if algorithm == "peak_ewma":
            print(f"Peak EWMA decay time: {args.peak_ewma_decay_time}")
        print("="*40)
        try:
            benchmark = MinimalBenchmark(algorithm, args.requests, args.warmup, args.envoy_binary_path, args.peak_ewma_decay_time)
            result = benchmark.run()
            if "error" not in result:
                results.append(result)
                print(result["report_text"])
            else:
                print(f"Error: {result['error']}")
        except Exception as e:
            print(f"\n--- ERROR for {algorithm.upper()} ---", file=sys.stderr)
            print(f"Benchmark failed: {e}", file=sys.stderr)
            # If envoy process exists, print its output
            if 'benchmark' in locals() and benchmark.envoy_process:
                stdout, stderr = benchmark.envoy_process.communicate()
                if stdout:
                    print(f"--> Envoy stdout:\n{stdout.decode()}", file=sys.stderr)
                if stderr:
                    print(f"--> Envoy stderr:\n{stderr.decode()}", file=sys.stderr)
    
    # Print summary table
    if results:
        print("\n" + "="*80)
        print("BENCHMARK SUMMARY")
        print("="*80)
        
        table_data = []
        for result in results:
            table_data.append([
                result["algorithm"],
                f"{result['average']:.2f}",
                f"{result['p50']:.2f}",
                f"{result['p75']:.2f}",
                f"{result['p90']:.2f}",
                f"{result['p95']:.2f}",
                f"{result['p99']:.2f}",
                f"{result['p999']:.2f}",
                f"{result['min']:.2f}",
                f"{result['max']:.2f}",
                f"{result['std_dev']:.2f}"
            ])
        
        headers = ["Algorithm", "Average", "P50", "P75", "P90", "P95", "P99", "P99.9", "Min", "Max", "Std Dev"]
        print(tabulate(table_data, headers=headers, tablefmt="grid"))
        print("\nAll latency values are in milliseconds.")


if __name__ == "__main__":
    main()
