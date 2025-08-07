#pragma once

#include "envoy/upstream/load_balancer.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/thread_local/thread_local_object.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"

#include "source/extensions/load_balancing_policies/common/load_balancer_impl.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/ewma.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/host_data.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/cost_calculator.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/stats_publisher.h"
#include "source/common/common/thread.h"
#include "source/common/common/callback_impl.h"

#include "absl/container/flat_hash_map.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <new>
#include <deque>

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

// Forward declarations and type aliases
using Config = envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma;
class WorkerLocalLbFactory;

constexpr size_t kCacheLineAlignment = 64;
constexpr int64_t kDefaultDecayTimeSeconds = 10;
constexpr double kDefaultRttMilliseconds = 10.0;  // Default RTT for new hosts (10ms)
constexpr int kPrefetchHighLocality = 3;
constexpr int kPrefetchReadHint = 0;


namespace {
class PeakEwmaTestPeer;
} // namespace

class PeakEwmaLoadBalancerFactory;
class PeakEwmaLoadBalancer;
class CostCalculator;
struct GlobalHostStats;

// Snapshot of EWMA values for all hosts, published atomically to workers
struct HostEwmaSnapshot {
  absl::flat_hash_map<Upstream::HostConstSharedPtr, double> ewma_values;
  uint64_t computation_timestamp_ns;
  uint64_t sequence_number; // Debugging: Track snapshot ordering in ThreadAware architecture
  double default_ewma_ms; // Fallback for hosts not in snapshot
  
  HostEwmaSnapshot(double default_ewma_ms, uint64_t timestamp_ns, uint64_t sequence)
      : computation_timestamp_ns(timestamp_ns), sequence_number(sequence), default_ewma_ms(default_ewma_ms) {}
};









/**
 * Peak EWMA Load Balancer with Host-Attached Atomic Storage.
 * Follows CSWRR pattern: stores data in Host objects, accessible from all threads.
 * Main thread processes samples, workers read EWMA values for P2C selection.
 */
class PeakEwmaLoadBalancer : public Upstream::LoadBalancerBase {
public:
  PeakEwmaLoadBalancer(
      const Upstream::PrioritySet& priority_set, const Upstream::PrioritySet* local_priority_set,
      Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
      uint32_t healthy_panic_threshold, const Upstream::ClusterInfo& cluster_info,
      TimeSource& time_source,
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config,
      Event::Dispatcher& main_dispatcher);

  ~PeakEwmaLoadBalancer();

  // LoadBalancer interface (direct implementation, no factory needed)
  Upstream::HostSelectionResponse chooseHost(Upstream::LoadBalancerContext* context) override;
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext* context) override;

private:
  friend struct GlobalHostStats;

  // Host management (following CSWRR pattern)  
  void addPeakEwmaLbPolicyDataToHosts(const Upstream::HostVector& hosts);
  PeakEwmaHostLbPolicyData* getPeakEwmaData(Upstream::HostConstSharedPtr host);
  
  // Timer-based aggregation (simplified - no cross-thread complexity)
  void aggregateWorkerData();
  void processHostSamples(Upstream::HostConstSharedPtr host, PeakEwmaHostLbPolicyData* data);
  void onAggregationTimer();
  
  // Power of Two Choices selection
  Upstream::HostConstSharedPtr selectFromTwoCandidates(
      const Upstream::HostVector& hosts, uint64_t random_value);
  double calculateHostCost(Upstream::HostConstSharedPtr host);
  
  // Core infrastructure
  const Upstream::PrioritySet& priority_set_;
  const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_proto_;
  Random::RandomGenerator& random_;
  
  // Business logic components
  CostCalculator cost_calculator_;
  StatsPublisher stats_publisher_;
  
  // Timer infrastructure for EWMA updates  
  Event::Dispatcher& main_dispatcher_;
  Event::TimerPtr aggregation_timer_;
  const std::chrono::milliseconds aggregation_interval_;
  
  // Host stats for admin interface visibility
  std::unordered_map<Upstream::HostConstSharedPtr, std::unique_ptr<GlobalHostStats>> all_host_stats_;
  
  // Priority set callback for adding data to new hosts (like CSWRR)
  Common::CallbackHandlePtr priority_update_cb_;
  
  // EWMA calculation constants
  const int64_t tau_nanos_;  // Decay time in nanoseconds
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
