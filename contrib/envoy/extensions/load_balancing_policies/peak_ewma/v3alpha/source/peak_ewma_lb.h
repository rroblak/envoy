#pragma once

#include "envoy/upstream/load_balancer.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/thread_local/thread_local_object.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"

#include "source/extensions/load_balancing_policies/common/load_balancer_impl.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/ewma.h"
#include "source/common/common/thread.h"

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

constexpr size_t kCacheLineAlignment = 64;
constexpr int64_t kDefaultDecayTimeSeconds = 10;
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
  double default_ewma_ms; // Fallback for hosts not in snapshot
  
  HostEwmaSnapshot(double default_ewma_ms, uint64_t timestamp_ns)
      : computation_timestamp_ns(timestamp_ns), default_ewma_ms(default_ewma_ms) {}
};


// Stats publisher for admin interface visibility
class StatsPublisher {
public:
  StatsPublisher(Stats::Scope& scope, TimeSource& time_source, const CostCalculator& cost_calculator, double default_rtt_ms)
      : scope_(scope), time_source_(time_source), cost_calculator_(cost_calculator), default_rtt_ms_(default_rtt_ms) {}
  
  // Publish all per-host stats: EWMA, active requests, and costs
  void publishHostStats(std::shared_ptr<HostEwmaSnapshot> snapshot, 
                       std::unordered_map<Upstream::HostConstSharedPtr, std::unique_ptr<GlobalHostStats>>& all_host_stats);

private:
  Stats::Scope& scope_;
  TimeSource& time_source_;
  const CostCalculator& cost_calculator_;
  double default_rtt_ms_;
  
  // Create host stats if they don't exist
  std::unique_ptr<GlobalHostStats> createHostStats(Upstream::HostConstSharedPtr host);
};

// Simplified host statistics for RTT collection interface
struct GlobalHostStats : public Upstream::HostLbPolicyData {
public:
  GlobalHostStats(Upstream::HostConstSharedPtr host, Stats::Scope& scope, TimeSource& time_source);
  ~GlobalHostStats() override = default;
  
  // Record RTT sample in thread-local circular buffers (lock-free)
  void recordRttSample(std::chrono::milliseconds rtt);
  
  // Set the load balancer reference for thread-local recording
  void setLoadBalancer(PeakEwmaLoadBalancer* lb) { load_balancer_ = lb; }
  
  // For observability - publish all three stats
  void setComputedCostStat(double cost);
  void setEwmaRttStat(double ewma_rtt_ms);
  void setActiveRequestsStat(double active_requests);

private:
  TimeSource& time_source_;
  Stats::Gauge& cost_stat_;
  Stats::Gauge& ewma_rtt_stat_;
  Stats::Gauge& active_requests_stat_;
  Upstream::HostConstSharedPtr host_;
  PeakEwmaLoadBalancer* load_balancer_{nullptr};
};

// RTT sample data for aggregation
struct RttSample {
  double rtt_ms;
  uint64_t timestamp_ns;
  
  RttSample() = default;
  RttSample(double rtt, uint64_t timestamp) : rtt_ms(rtt), timestamp_ns(timestamp) {}
};

// Pure business logic for cost calculation
class CostCalculator {
public:
  static constexpr double kPenaltyValue = 1000000.0;
  
  // Calculate host cost based on EWMA RTT and active requests
  // Uses default_rtt_ms when no RTT measurements are available
  double calculateCost(double rtt_ewma_ms, double active_requests, double default_rtt_ms) const;
};

// Power of Two Choices selection algorithm  
class PowerOfTwoSelector {
public:
  static constexpr uint64_t kTieBreakingMask = 0x1;
  
  struct HostCostPair {
    Upstream::HostConstSharedPtr host;
    double cost;
  };
  
  // Select the best host from two candidates using P2C algorithm
  Upstream::HostConstSharedPtr selectBest(
      Upstream::HostConstSharedPtr first_host, double first_cost,
      Upstream::HostConstSharedPtr second_host, double second_cost,
      uint64_t random_value) const;
      
  // Generate two distinct host indices using optimized single random call
  std::pair<size_t, size_t> generateTwoDistinctIndices(size_t host_count, uint64_t random_value) const;
};


// Single-buffer design: each worker has one buffer for all hosts
// Napkin math for 100K buffer size:
// - Peak scenario: 1M RPS per worker, 100ms aggregation = 100K samples  
// - Conservative: 500K RPS per worker = 50K samples per cycle
// - Safety margin: 2x = 100K samples
// - Memory: 100K samples × 24 bytes = ~2.4 MB per worker
// - Total: 8 workers × 2.4 MB = ~19 MB (vs 600+ MB previously)
struct PerThreadData : public ThreadLocal::ThreadLocalObject {
public:
  PerThreadData() : active_buffer_(&buffer_a_), write_pos_(0) {
    buffer_a_.reserve(kMaxSamplesPerWorker);
    buffer_b_.reserve(kMaxSamplesPerWorker);
  }
  
  // Record RTT sample for any host - single buffer for all hosts
  void recordRttSample(Upstream::HostConstSharedPtr host, std::chrono::milliseconds rtt, uint64_t timestamp_ns);
  
  // Simple buffer swap: switch to other buffer, clear it, return old buffer pointer  
  std::vector<std::pair<Upstream::HostConstSharedPtr, RttSample>>* swapAndClearBuffer();

private:
  // Conservative buffer size: handles extreme loads while using minimal memory
  static constexpr uint32_t kMaxSamplesPerWorker = 100000;
  
  // Double buffer design for single worker buffer
  std::vector<std::pair<Upstream::HostConstSharedPtr, RttSample>> buffer_a_;
  std::vector<std::pair<Upstream::HostConstSharedPtr, RttSample>> buffer_b_;
  std::vector<std::pair<Upstream::HostConstSharedPtr, RttSample>>* active_buffer_;
  size_t write_pos_;  // Circular write position
};

class PeakEwmaLoadBalancer : public Upstream::ZoneAwareLoadBalancerBase {
public:
  PeakEwmaLoadBalancer(
      const Upstream::PrioritySet& priority_set, const Upstream::PrioritySet* local_priority_set,
      Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
      uint32_t healthy_panic_threshold, const Upstream::ClusterInfo& cluster_info,
      TimeSource& time_source,
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config,
      Event::Dispatcher& main_dispatcher, ThreadLocal::TypedSlot<PerThreadData>& tls_slot);

  ~PeakEwmaLoadBalancer();

  Upstream::HostConstSharedPtr chooseHostOnce(Upstream::LoadBalancerContext* context) override;
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext* context) override;

private:
  friend struct GlobalHostStats;

  using HostCostPair = std::pair<Upstream::HostConstSharedPtr, double>;
  
  // Pure business logic helpers
  CostCalculator cost_calculator_;
  PowerOfTwoSelector p2c_selector_;
  StatsPublisher stats_publisher_;

  Upstream::HostConstSharedPtr selectFromTwoCandidates(
      const Upstream::HostVector& hosts, uint64_t random_value);
  double calculateHostCost(Upstream::HostConstSharedPtr host);
  
  void onHostSetUpdate(const Upstream::HostVector& hosts_added,
                       const Upstream::HostVector& hosts_removed);
  
  int64_t getCachedTimeNanos() const;

  // Thread-local storage access methods
  PerThreadData& getThreadLocalData();

  // Timer-based aggregation methods (Phase 4)
  void aggregateWorkerData();
  void processCollectedData(std::shared_ptr<std::vector<std::vector<std::pair<Upstream::HostConstSharedPtr, RttSample>>*>> collected_buffers);
  void onAggregationTimer();
  void startAggregationTimer();

  // Hybrid EWMA snapshot methods
  static double getEwmaFromSnapshot(std::shared_ptr<HostEwmaSnapshot> snapshot, Upstream::HostConstSharedPtr host);

  const Upstream::ClusterInfo& cluster_info_;
  TimeSource& time_source_;
  const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_proto_;
  const int64_t tau_nanos_;
  Common::CallbackHandlePtr member_update_cb_handle_;
  
  // Thread-local storage for per-worker statistics (owned by config)
  ThreadLocal::TypedSlot<PerThreadData>& tls_slot_;
  
  // Timer-based aggregation infrastructure (Phase 4)
  Event::Dispatcher& main_dispatcher_;
  Event::TimerPtr aggregation_timer_;
  const std::chrono::milliseconds aggregation_interval_;
  
  // Time caching optimization - reduce syscall overhead
  mutable int64_t cached_time_nanos_ = 0;
  mutable uint32_t time_cache_counter_ = 0;
  static constexpr uint32_t kTimeCacheUpdates = 16;
  
public:
  // Hybrid EWMA pre-computation: atomic shared_ptr for race-free worker access  
  // Public for testing - using C++11 compatible free functions
  std::shared_ptr<HostEwmaSnapshot> current_ewma_snapshot_;

private:
  
  // Host stats for admin interface visibility
  std::unordered_map<Upstream::HostConstSharedPtr, std::unique_ptr<GlobalHostStats>> all_host_stats_;
  
  // Timer state tracking to prevent redundant starts/stops
  bool aggregation_timer_started_{false};
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
