# Peak EWMA Load Balancer Integration Test

This directory contains integration tests for the Peak EWMA load balancer that run against actual Envoy containers.

## Overview

The test suite validates Peak EWMA functionality by:

1. **Basic Load Distribution**: Verifies requests are distributed across multiple upstreams using P2C selection
2. **Latency-Aware Routing**: Tests that Peak EWMA automatically routes away from slower upstreams  
3. **Configuration Validation**: Ensures Peak EWMA configuration is accepted and functional
4. **Stats Verification**: Checks Envoy admin interface for load balancer metrics

## Test Environment

The test uses Docker Compose to create:

- **Envoy Proxy**: Configured with Peak EWMA load balancer
- **3 Upstream Servers**: Nginx servers simulating different performance characteristics
  - `upstream_fast`: Normal response times
  - `upstream_medium`: Baseline response times  
  - `upstream_slow`: Target for artificial load to create latency differences

## Prerequisites

- Docker and Docker Compose
- Python 3.6+ with `requests` library
- Envoy contrib image with Peak EWMA support

## Running the Tests

### Method 1: Automated Integration Test

Run the complete automated test suite:

```bash
cd test/contrib/peak_ewma_integration
python3 test_peak_ewma.py
```

This script will:
- Start all services with Docker Compose
- Run load distribution tests  
- Create artificial latency differences
- Measure Peak EWMA's response to latency changes
- Display detailed results and cleanup

### Method 2: Manual Testing

Start the environment manually for interactive testing:

```bash
# Start services
docker-compose up -d

# Wait for Envoy to start (check logs)
docker-compose logs envoy

# Send test requests
curl http://localhost:8080/

# Check Envoy admin interface
curl http://localhost:9901/stats | grep cluster.test_cluster

# View cluster endpoints and health
curl http://localhost:9901/clusters

# Cleanup when done
docker-compose down
```

### Method 3: Load Testing with External Tools

Use load testing tools to generate realistic traffic patterns:

```bash
# Start services
docker-compose up -d

# Install wrk (if not available)
# brew install wrk  # macOS
# apt-get install wrk  # Ubuntu

# Generate load to observe Peak EWMA behavior
wrk -t10 -c50 -d30s http://localhost:8080/

# Check stats during and after load
curl http://localhost:9901/stats?filter=cluster.test_cluster
```

## Test Configuration

The `envoy.yaml` configuration includes:

- **Peak EWMA Settings**:
  - `rtt_smoothing_factor: 0.3` (default, balanced responsiveness)
  - `default_rtt: 50ms` (conservative baseline)

- **Health Checking**: Ensures only healthy upstreams receive traffic
- **Outlier Detection**: Removes consistently failing upstreams

## Expected Results

### Successful Test Indicators

1. **Request Distribution**: Traffic distributed across multiple upstreams (not just one)
2. **Latency Sensitivity**: Reduced traffic to artificially slowed upstreams
3. **Configuration Acceptance**: Envoy starts without configuration errors
4. **Health Integration**: Health checking and outlier detection work alongside Peak EWMA

### Key Metrics to Monitor

- `cluster.test_cluster.upstream_rq_total`: Total requests per upstream
- `cluster.test_cluster.lb_healthy_panic`: Should remain 0 (no panic mode)
- Response time distribution across upstreams
- Envoy admin `/clusters` endpoint showing endpoint health

## Troubleshooting

### Envoy Won't Start
- Check if Peak EWMA is available in the Envoy image: `envoyproxy/envoy:contrib-v1.28-latest`
- Verify configuration syntax: `docker-compose logs envoy`

### No Load Distribution
- Verify all upstreams are healthy: `curl http://localhost:9901/clusters`
- Check upstream connectivity: `curl http://localhost:8081/health`

### Peak EWMA Not Responding to Latency
- Increase test duration to allow EWMA convergence
- Verify artificial load is creating measurable latency differences
- Check `rtt_smoothing_factor` - higher values are more sensitive (default 0.3)

## Integration with CI/CD

This test can be integrated into CI/CD pipelines:

```bash
# In CI script
cd test/contrib/peak_ewma_integration
python3 test_peak_ewma.py
exit_code=$?

if [ $exit_code -eq 0 ]; then
    echo "Peak EWMA integration test PASSED"
else
    echo "Peak EWMA integration test FAILED"
    exit 1
fi
```

## Extending the Tests

To add more test scenarios:

1. **Different Latency Patterns**: Modify the load generation in `test_peak_ewma.py`
2. **Configuration Variations**: Create additional `envoy-*.yaml` files 
3. **Error Scenarios**: Add upstreams that return errors to test outlier detection integration
4. **Scaling Tests**: Increase the number of upstreams to test P2C behavior at scale