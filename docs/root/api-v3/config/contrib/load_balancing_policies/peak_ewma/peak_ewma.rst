Peak EWMA Load Balancer
========================

.. note::

  Peak EWMA is a contrib extension that must be explicitly enabled at Envoy build time.
  See :ref:`install_contrib` for details.

The Peak EWMA (Exponentially Weighted Moving Average) load balancer implements a latency-aware 
variant of the Power of Two Choices (P2C) algorithm. It automatically routes traffic to the 
best-performing hosts based on real-time latency measurements and current load.

.. important::

  Peak EWMA only considers latency and load when making routing decisions. It does **not** handle
  unhealthy hosts or error responses directly. Use Envoy's :ref:`health checking 
  <arch_overview_health_checking>` and :ref:`outlier detection <arch_overview_outlier_detection>` 
  to manage host health and automatically remove failing hosts from the load balancing pool.

Algorithm Overview
------------------

Peak EWMA uses the cost function: ``Cost = RTT_peak_ewma * (active_requests + 1)``

Key characteristics:

* **Latency-sensitive**: Automatically de-prioritizes slow hosts
* **Load-aware**: Considers both latency and current request count  
* **O(1) complexity**: Efficient P2C selection scales to large clusters
* **Adaptive**: No manual tuning required, responds to performance changes
* **Health-agnostic**: Operates only on healthy hosts as determined by health checking and outlier detection

Integration with Health Management
----------------------------------

Peak EWMA works in conjunction with Envoy's health management systems:

* **Health Checking**: Only hosts that pass active health checks are considered for load balancing
* **Outlier Detection**: Hosts ejected by outlier detection are automatically excluded from selection
* **Error Handling**: HTTP error responses (4xx/5xx) do not directly affect Peak EWMA routing decisions

For comprehensive host health management, configure Peak EWMA alongside:

.. code-block:: yaml

  cluster:
    # Health checking removes unresponsive hosts
    health_checks:
    - timeout: 5s
      interval: 10s
      http_health_check:
        path: "/health"
    
    # Outlier detection removes hosts with high error rates
    outlier_detection:
      consecutive_5xx: 3
      interval: 30s
      base_ejection_time: 30s
    
    # Peak EWMA optimizes among remaining healthy hosts
    load_balancing_policy:
      policies:
      - typed_extension_config:
          name: envoy.load_balancing_policies.peak_ewma
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.load_balancing_policies.peak_ewma.v3alpha.PeakEwma
            rtt_smoothing_factor: 0.1
            default_rtt: 100ms

Configuration Example
---------------------

Basic Peak EWMA configuration:

.. code-block:: yaml

  load_balancing_policy:
    policies:
    - typed_extension_config:
        name: envoy.load_balancing_policies.peak_ewma
        typed_config:
          "@type": type.googleapis.com/envoy.extensions.load_balancing_policies.peak_ewma.v3alpha.PeakEwma
          rtt_smoothing_factor: 0.1
          default_rtt: 100ms

API Reference
-------------

.. toctree::
  :glob:
  :maxdepth: 2

  ../../../../../extensions/load_balancing_policies/peak_ewma/v3alpha/*