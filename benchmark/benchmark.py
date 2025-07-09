#!/usr/bin/env python3
"""
Envoy Load Balancer Benchmark Tool with nginx upstream servers

Uses nginx servers instead of Python HTTP servers to eliminate any threading/concurrency issues.
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
import threading
import numpy as np
from tabulate import tabulate
import shutil
import random
import yaml

class NginxUpstreamManager:
    def __init__(self, num_fast_servers: int = 9, num_slow_servers: int = 1, 
                 fast_delay_ms: int = 5, slow_delay_ms: int = 100, base_port: int = 19000):
        self.num_fast_servers = num_fast_servers
        self.num_slow_servers = num_slow_servers
        self.fast_delay_ms = fast_delay_ms
        self.slow_delay_ms = slow_delay_ms
        self.base_port = base_port
        self.nginx_processes = []
        self.temp_dirs = []

    def create_nginx_config(self) -> str:
        """Create single nginx config with multiple server blocks"""
        server_blocks = []
        
        # Create fast servers
        for i in range(self.num_fast_servers):
            port = self.base_port + i
            server_name = f"fast_{i}"
            server_blocks.append(f"""
    server {{
        listen {port};
        
        location / {{
            access_by_lua_block {{
                ngx.sleep({self.fast_delay_ms / 1000.0})
            }}
            
            content_by_lua_block {{
                ngx.header["Content-Type"] = "application/json"
                ngx.say('{{"server": "{server_name}", "delay_ms": {self.fast_delay_ms}}}')
            }}
        }}
    }}""")
        
        # Create slow server
        slow_port = self.base_port + self.num_fast_servers
        server_blocks.append(f"""
    server {{
        listen {slow_port};
        
        location / {{
            access_by_lua_block {{
                ngx.sleep({self.slow_delay_ms / 1000.0})
            }}
            
            content_by_lua_block {{
                ngx.header["Content-Type"] = "application/json"
                ngx.say('{{"server": "slow", "delay_ms": {self.slow_delay_ms}}}')
            }}
        }}
    }}""")
        
        config = f"""
worker_processes 1;
error_log /tmp/nginx_unified_error.log info;
pid /tmp/nginx_unified.pid;

# Load required modules (NDK must be loaded before Lua)
load_module /usr/lib/nginx/modules/ndk_http_module.so;
load_module /usr/lib/nginx/modules/ngx_http_lua_module.so;

events {{
    worker_connections 1024;
}}

http {{
    access_log /tmp/nginx_unified_access.log;
    
{''.join(server_blocks)}
}}
"""
        return config

    def start_all_servers(self):
        """Start single nginx process with multiple server blocks"""
        print("Starting nginx upstream servers...")
        
        temp_dir = tempfile.mkdtemp()
        self.temp_dirs.append(temp_dir)
        
        config_path = os.path.join(temp_dir, 'nginx.conf')
        config_content = self.create_nginx_config()
        
        with open(config_path, 'w') as f:
            f.write(config_content)
        
        # Start single nginx process
        cmd = ['nginx', '-c', config_path, '-g', 'daemon off;']
        process = subprocess.Popen(
            cmd, 
            stdout=subprocess.PIPE, 
            stderr=subprocess.PIPE,
            preexec_fn=os.setsid,
            text=True
        )
        self.nginx_processes.append(process)
        
        # Wait a bit for nginx to start
        time.sleep(1)
        
        # Check if process is still running
        if process.poll() is not None:
            try:
                stdout, stderr = process.communicate()
                stdout = stdout or "No stdout"
                stderr = stderr or "No stderr"
            except Exception as comm_e:
                stdout, stderr = f"Communication error: {comm_e}", "N/A"
            raise RuntimeError(f"nginx unified server failed to start.\nSTDOUT: {stdout}\nSTDERR: {stderr}")
        
        # Verify all ports are responding
        all_ports = list(range(self.base_port, self.base_port + self.num_fast_servers + self.num_slow_servers))
        for port in all_ports:
            try:
                response = requests.get(f"http://127.0.0.1:{port}/", timeout=1)
                print(f"nginx server on port {port} responding")
            except Exception as e:
                # Get any available output
                try:
                    stdout, stderr = process.communicate(timeout=1)
                    stdout = stdout or "No stdout"
                    stderr = stderr or "No stderr"
                except:
                    stdout, stderr = "Communication timeout", "N/A"
                raise RuntimeError(f"nginx server on port {port} failed to respond: {e}\nSTDOUT: {stdout}\nSTDERR: {stderr}")
        
        print(f"Single nginx process started with {self.num_fast_servers} fast and {self.num_slow_servers} slow server blocks.")

    def stop_all_servers(self):
        """Stop all nginx servers and cleanup"""
        for process in self.nginx_processes:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
                process.wait(timeout=5)
            except:
                try:
                    os.killpg(os.getpgid(process.pid), signal.SIGKILL)
                except:
                    pass
        
        for temp_dir in self.temp_dirs:
            try:
                shutil.rmtree(temp_dir)
            except:
                pass

class EnvoyBenchmark:
    def __init__(self, algorithm: str, num_requests: int = 2000, warmup_requests: int = 200, 
                 envoy_binary_path: str = "", peak_ewma_decay_time: str = "10s"):
        self.algorithm = algorithm
        self.num_requests = num_requests
        self.warmup_requests = warmup_requests
        self.peak_ewma_decay_time = peak_ewma_decay_time
        self.fast_delay_ms = 5
        self.slow_delay_ms = 100
        self.num_fast_servers = 9
        self.num_slow_servers = 1
        self.envoy_process = None
        self.server_requests = {}
        self.response_times = []
        self.base_port = 19000
        self.envoy_port = 8081
        self.envoy_admin_port = 9902
        self.envoy_binary_path = envoy_binary_path
        self.nginx_manager = NginxUpstreamManager()

    def get_envoy_binary_path(self) -> str:
        if self.envoy_binary_path:
            return os.path.abspath(self.envoy_binary_path)
        # Default to the contrib build binary
        default_path = "/srv/apps/envoy/linux/amd64/build_envoy-contrib_fastbuild/envoy"
        if os.path.exists(default_path):
            return default_path
        raise RuntimeError("Envoy binary path not provided and default contrib binary not found.")

    def create_envoy_config(self, temp_dir: str) -> str:
        """Create Envoy configuration file"""
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
        print(f"Envoy binary path being used: {envoy_binary}")
        
        self.envoy_process = subprocess.Popen([
            envoy_binary, '-c', config_path, '--log-level', 'error'
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        
        # Wait for Envoy to start
        time.sleep(2)
        try:
            requests.get(f"http://127.0.0.1:{self.envoy_port}/", timeout=1)
        except requests.exceptions.RequestException:
            stdout, stderr = self.envoy_process.communicate()
            stderr_msg = stderr if stderr else "No stderr output"
            stdout_msg = stdout if stdout else "No stdout output"
            raise RuntimeError(f"Envoy failed to start.\nSTDOUT: {stdout_msg}\nSTDERR: {stderr_msg}")

    def stop_envoy(self) -> None:
        if self.envoy_process:
            self.envoy_process.terminate()
            self.envoy_process.wait(timeout=5)

    def send_request(self) -> Optional[Tuple[str, float]]:
        start_time = time.time()
        try:
            response = requests.get(f"http://127.0.0.1:{self.envoy_port}/", timeout=5)
            if response.status_code == 200:
                server_name = response.json()["server"]
                return server_name, (time.time() - start_time) * 1000
        except requests.exceptions.RequestException as e:
            print(f"Request failed: {e}")
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

    def generate_report(self) -> dict:
        if not self.response_times: 
            return {"error": "No data collected."}
        
        # Debug: Print all server names received
        print(f"DEBUG: All server responses received: {dict(self.server_requests)}")
        
        successful_requests = sum(self.server_requests.values())
        fast_reqs = sum(self.server_requests.get(f'fast_{i}', 0) for i in range(self.num_fast_servers))
        slow_reqs = self.server_requests.get('slow', 0)
        
        print(f"DEBUG: Fast server requests: {fast_reqs}, Slow server requests: {slow_reqs}, Total: {successful_requests}")
        
        # Calculate detailed statistics
        times = np.array(self.response_times)
        stats = {
            "algorithm": self.algorithm,
            "total_requests": successful_requests,
            "fast_requests": fast_reqs,
            "slow_requests": slow_reqs,
            "fast_percentage": (fast_reqs / successful_requests * 100) if successful_requests > 0 else 0,
            "slow_percentage": (slow_reqs / successful_requests * 100) if successful_requests > 0 else 0,
            "average": float(np.mean(times)),
            "min": float(np.min(times)),
            "max": float(np.max(times)),
            "std_dev": float(np.std(times)),
            "p50": float(np.percentile(times, 50)),
            "p75": float(np.percentile(times, 75)),
            "p90": float(np.percentile(times, 90)),
            "p95": float(np.percentile(times, 95)),
            "p99": float(np.percentile(times, 99))
        }
        
        # Generate analysis
        expected_slow_percentage = (self.num_slow_servers / (self.num_fast_servers + self.num_slow_servers)) * 100
        slow_analysis = "✅ Algorithm effectively avoided the slow server." if stats["slow_percentage"] < expected_slow_percentage * 1.5 else "❌ Algorithm did not effectively avoid the slow server."
        
        report_text = f"""
--- Report for {self.algorithm.upper()} ---
Avg Latency: {stats['average']:.2f}ms
P95 Latency: {stats['p95']:.2f}ms
Request Distribution:
  - Fast Servers: {fast_reqs} requests ({stats['fast_percentage']:.1f}%)
  - Slow Server: {slow_reqs} requests ({stats['slow_percentage']:.1f}%)
ANALYSIS: {slow_analysis}
"""
        
        stats["report_text"] = report_text.strip()
        return stats

    def run(self) -> dict:
        try:
            with tempfile.TemporaryDirectory() as temp_dir:
                self.nginx_manager.start_all_servers()
                config_path = self.create_envoy_config(temp_dir)
                self.start_envoy(config_path)
                self.run_phase("warmup", self.warmup_requests)
                self.run_phase("benchmark", self.num_requests)
                return self.generate_report()
        finally:
            self.stop_envoy()
            self.nginx_manager.stop_all_servers()

def main():
    parser = argparse.ArgumentParser(description="Envoy Load Balancer Benchmark with nginx upstream servers")
    parser.add_argument("--algorithm", "-a", required=True, help="LB algorithm(s) to test (comma-separated)")
    parser.add_argument("--requests", "-r", type=int, default=2000, help="Number of benchmark requests")
    parser.add_argument("--warmup", "-w", type=int, default=200, help="Number of warmup requests")
    parser.add_argument("--envoy_binary_path", type=str, default="", help="Path to the Envoy binary")
    parser.add_argument("--peak_ewma_decay_time", type=str, default="10s", help="Peak EWMA decay time")
    args = parser.parse_args()

    # Check if nginx is available
    if not shutil.which('nginx'):
        print("Error: nginx is not installed or not in PATH. Please install nginx first.")
        sys.exit(1)

    algorithms = [algo.strip() for algo in args.algorithm.split(',')]
    
    results = []
    
    for algorithm in algorithms:
        print("="*40)
        print(f"BENCHMARKING: {algorithm.upper()}")
        if algorithm == "peak_ewma":
            print(f"Peak EWMA decay time: {args.peak_ewma_decay_time}")
        print("="*40)
        try:
            benchmark = EnvoyBenchmark(algorithm, args.requests, args.warmup, args.envoy_binary_path, args.peak_ewma_decay_time)
            result = benchmark.run()
            if "error" not in result:
                results.append(result)
                print(result["report_text"])
            else:
                print(f"Error: {result['error']}")
        except Exception as e:
            print(f"\n--- ERROR for {algorithm.upper()} ---", file=sys.stderr)
            print(f"Benchmark failed: {e}", file=sys.stderr)
    
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
                f"{result['min']:.2f}",
                f"{result['max']:.2f}",
                f"{result['std_dev']:.2f}"
            ])
        
        headers = ["Algorithm", "Average", "P50", "P75", "P90", "P95", "P99", "Min", "Max", "Std Dev"]
        print(tabulate(table_data, headers=headers, tablefmt="grid"))
        print("\nAll latency values are in milliseconds.")

if __name__ == "__main__":
    main()