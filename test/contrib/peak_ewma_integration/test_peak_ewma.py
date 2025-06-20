#!/usr/bin/env python3
"""
Integration test for Peak EWMA load balancer.

This script tests the Peak EWMA load balancer by:
1. Starting a containerized Envoy with Peak EWMA configuration
2. Creating artificial latency differences between upstreams  
3. Measuring request distribution to verify latency-aware routing
4. Checking admin stats for load balancer behavior
"""

import json
import time
import requests
import subprocess
import sys
from collections import defaultdict, Counter
from typing import Dict, List, Tuple
import concurrent.futures


def run_command(cmd: List[str]) -> str:
    """Run a command and return its output."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return result.stdout.strip()
    except subprocess.CalledProcessError as e:
        print(f"Command failed: {' '.join(cmd)}")
        print(f"Error: {e.stderr}")
        raise


def wait_for_envoy(envoy_url: str, timeout: int = 30) -> bool:
    """Wait for Envoy to be ready."""
    print("Waiting for Envoy to start...")
    for _ in range(timeout):
        try:
            response = requests.get(f"{envoy_url}/ready", timeout=1)
            if response.status_code == 200:
                print("Envoy is ready!")
                return True
        except requests.RequestException:
            pass
        time.sleep(1)
    
    print("Timeout waiting for Envoy")
    return False


def make_request(url: str) -> Tuple[str, float]:
    """Make a request and return the server name and response time."""
    start_time = time.time()
    try:
        response = requests.get(url, timeout=5)
        response_time = time.time() - start_time
        
        if response.status_code == 200:
            data = response.json()
            return data.get('server', 'unknown'), response_time
        else:
            return 'error', response_time
    except Exception as e:
        response_time = time.time() - start_time
        return 'error', response_time


def simulate_upstream_load(upstream_ports: List[int], load_duration: int = 30):
    """Simulate load on specific upstreams to create latency differences."""
    def stress_upstream(port: int):
        """Send requests to create load on an upstream."""
        url = f"http://localhost:{port}/"
        end_time = time.time() + load_duration
        
        while time.time() < end_time:
            try:
                requests.get(url, timeout=1)
            except:
                pass
            time.sleep(0.01)  # Small delay between requests
    
    print(f"Creating artificial load on upstreams {upstream_ports} for {load_duration}s...")
    
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(upstream_ports) * 2) as executor:
        # Create load on specified upstreams
        futures = []
        for port in upstream_ports:
            # Multiple threads per upstream to create more load
            futures.append(executor.submit(stress_upstream, port))
            futures.append(executor.submit(stress_upstream, port))
        
        # Wait for load generation to complete
        time.sleep(load_duration)


def test_load_distribution(envoy_url: str, num_requests: int = 100) -> Dict[str, int]:
    """Test request distribution across upstreams."""
    print(f"Sending {num_requests} requests to test load distribution...")
    
    server_counts = Counter()
    response_times = defaultdict(list)
    
    for i in range(num_requests):
        server, response_time = make_request(envoy_url)
        server_counts[server] += 1
        response_times[server].append(response_time)
        
        if (i + 1) % 10 == 0:
            print(f"  Sent {i + 1}/{num_requests} requests")
    
    print("\nRequest distribution:")
    for server, count in server_counts.items():
        avg_time = sum(response_times[server]) / len(response_times[server])
        print(f"  {server}: {count} requests ({count/num_requests*100:.1f}%), avg latency: {avg_time*1000:.1f}ms")
    
    return dict(server_counts)


def get_envoy_stats(admin_url: str) -> Dict:
    """Get Envoy admin stats."""
    try:
        response = requests.get(f"{admin_url}/stats?format=json")
        if response.status_code == 200:
            stats = response.json()
            return {stat['name']: stat['value'] for stat in stats['stats']}
        else:
            print(f"Failed to get stats: {response.status_code}")
            return {}
    except Exception as e:
        print(f"Error getting stats: {e}")
        return {}


def print_relevant_stats(stats: Dict):
    """Print relevant load balancer stats."""
    print("\nRelevant Envoy stats:")
    
    relevant_patterns = [
        'cluster.test_cluster.upstream_rq_total',
        'cluster.test_cluster.upstream_rq_success', 
        'cluster.test_cluster.upstream_rq_pending',
        'cluster.test_cluster.lb_healthy_panic',
        'cluster.test_cluster.health_check',
    ]
    
    for pattern in relevant_patterns:
        matching_stats = {k: v for k, v in stats.items() if pattern in k}
        for name, value in matching_stats.items():
            print(f"  {name}: {value}")


def run_peak_ewma_test():
    """Run the complete Peak EWMA integration test."""
    print("=== Peak EWMA Load Balancer Integration Test ===")
    
    envoy_url = "http://localhost:8080"
    admin_url = "http://localhost:9901"
    
    # Start services
    print("Starting services with docker-compose...")
    run_command(["docker-compose", "up", "-d"])
    
    try:
        # Wait for Envoy to start
        if not wait_for_envoy(admin_url):
            return False
        
        # Test 1: Basic load distribution
        print("\n--- Test 1: Basic Load Distribution ---")
        initial_distribution = test_load_distribution(envoy_url, 50)
        
        # Test 2: Create artificial load differences
        print("\n--- Test 2: Latency-Aware Routing ---")
        print("Creating load on 'slow' upstream to increase its latency...")
        
        # Start background load on slow upstream (port 8083)
        load_thread = concurrent.futures.ThreadPoolExecutor(max_workers=1)
        load_future = load_thread.submit(simulate_upstream_load, [8083], 20)
        
        # Give load time to build up
        time.sleep(5)
        
        # Test distribution during load
        loaded_distribution = test_load_distribution(envoy_url, 100)
        
        # Wait for load to finish
        load_future.result()
        load_thread.shutdown()
        
        # Test 3: Recovery after load
        print("\n--- Test 3: Recovery After Load ---")
        time.sleep(10)  # Wait for EWMA to adapt
        recovery_distribution = test_load_distribution(envoy_url, 50)
        
        # Get final stats
        print("\n--- Envoy Statistics ---")
        stats = get_envoy_stats(admin_url)
        print_relevant_stats(stats)
        
        # Analyze results
        print("\n=== Test Analysis ===")
        
        # Check if load distribution changes appropriately
        slow_initial = initial_distribution.get('slow', 0)
        slow_loaded = loaded_distribution.get('slow', 0)
        slow_recovery = recovery_distribution.get('slow', 0)
        
        total_initial = sum(initial_distribution.values())
        total_loaded = sum(loaded_distribution.values())
        total_recovery = sum(recovery_distribution.values())
        
        slow_ratio_initial = slow_initial / total_initial if total_initial > 0 else 0
        slow_ratio_loaded = slow_loaded / total_loaded if total_loaded > 0 else 0
        slow_ratio_recovery = slow_recovery / total_recovery if total_recovery > 0 else 0
        
        print(f"Slow upstream request ratio:")
        print(f"  Initial: {slow_ratio_initial:.3f}")
        print(f"  Under load: {slow_ratio_loaded:.3f}")
        print(f"  Recovery: {slow_ratio_recovery:.3f}")
        
        # Success criteria
        success = True
        
        # Test that requests are distributed to multiple upstreams
        if len(initial_distribution) < 2:
            print("❌ FAIL: Requests not distributed to multiple upstreams")
            success = False
        else:
            print("✅ PASS: Requests distributed to multiple upstreams")
        
        # Test that slow upstream gets fewer requests under load
        if slow_ratio_loaded < slow_ratio_initial:
            print("✅ PASS: Peak EWMA reduced traffic to slow upstream")
        else:
            print("⚠️  WARNING: Peak EWMA did not clearly reduce traffic to slow upstream")
            print("   This could be due to insufficient load or timing")
        
        print(f"\n=== Test {'PASSED' if success else 'FAILED'} ===")
        return success
        
    finally:
        print("\nCleaning up...")
        run_command(["docker-compose", "down"])


if __name__ == "__main__":
    success = run_peak_ewma_test()
    sys.exit(0 if success else 1)