#pragma once

#include "envoy/upstream/load_balancer.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/thread_local/thread_local_object.h"

#include "source/extensions/load_balancing_policies/common/load_balancer_impl.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/ewma.h"

#include "absl/container/flat_hash_map.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <new>
#include <deque>
#include <mutex>

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

constexpr size_t kCacheLineAlignment = 64;
constexpr int64_t kDefaultDecayTimeSeconds = 10;
constexpr double kPenaltyValue = static_cast<double>(std::numeric_limits<int64_t>::max() >> 16);  // Finagle-compatible penalty
constexpr int kPrefetchHighLocality = 3;
constexpr int kPrefetchReadHint = 0;
constexpr uint64_t kTieBreakingMask = 0x8000000000000000ULL;

namespace {
class PeakEwmaTestPeer;
} // namespace

class PeakEwmaLoadBalancerFactory;

// EWMA and timestamp data for atomic shared_ptr access
struct EwmaTimestampData {
  double ewma;
  uint64_t timestamp_ns;
  
  EwmaTimestampData(double ewma_val, uint64_t timestamp) 
    : ewma(ewma_val), timestamp_ns(timestamp) {}
};

// Global host statistics shared across all worker threads
struct alignas(kCacheLineAlignment) GlobalHostStats : public Upstream::HostLbPolicyData {
public:
  GlobalHostStats(const Upstream::Host& host, int64_t tau_nanos, 
                  Stats::Scope& scope, TimeSource& time_source);
  
  ~GlobalHostStats();
  
  // Non-copyable due to atomic members
  GlobalHostStats(const GlobalHostStats&) = delete;
  GlobalHostStats& operator=(const GlobalHostStats&) = delete;

  // Get current EWMA value (for load balancing decisions)
  double getEwmaRttMs() const;
  double getEwmaRttMs(int64_t cached_time_nanos) const;
  
  // Get current pending request count
  int64_t getPendingRequests() const { 
    return pending_requests.load(std::memory_order_relaxed); 
  }
  
  // Increment/decrement pending requests (called on request start/end)
  void incrementPendingRequests() { 
    pending_requests.fetch_add(1, std::memory_order_relaxed); 
  }
  void decrementPendingRequests() { 
    pending_requests.fetch_sub(1, std::memory_order_relaxed); 
  }

  // Update global EWMA from aggregated worker data (called by main thread)
  void updateGlobalEwma(double new_ewma, uint64_t timestamp_ns);
  
  // Record RTT sample directly (thread-safe, for compatibility during transition)
  void recordRttSample(std::chrono::milliseconds rtt);
  
  // For observability
  void setComputedCostStat(double cost) { cost_stat_.set(static_cast<uint64_t>(cost)); }

  // EWMA data with lock-free reads via atomic pointer
  std::atomic<const EwmaTimestampData*> current_ewma_data_;
  
  // Real-time count of in-flight requests across all threads
  std::atomic<int64_t> pending_requests{0};

private:
  // Configuration (immutable after creation)
  const double decay_constant_;
  const uint64_t default_rtt_ns_;
  TimeSource& time_source_;
  Stats::Gauge& cost_stat_;
  
  // Temporary: Direct EWMA calculator for RTT recording (will be replaced by aggregation in Phase 4)
  mutable std::mutex rtt_mutex_;
  mutable PeakEwmaCalculator direct_ewma_;
};

// Per-thread host statistics for worker thread data collection
struct PerThreadHostStats : public ThreadLocal::ThreadLocalObject {
public:
  PerThreadHostStats(Upstream::HostConstSharedPtr host, int64_t tau_nanos);
  
  // Record RTT sample in this worker thread
  void recordRttSample(std::chrono::milliseconds rtt, uint64_t timestamp_ns);
  
  // Get current thread-local EWMA value
  double getCurrentEwma(uint64_t timestamp_ns) const { 
    return const_cast<PeakEwmaCalculator&>(local_ewma_).value(timestamp_ns); 
  }
  
  // Get pending requests for this thread
  int64_t getLocalPendingRequests() const { return local_pending_requests_; }
  
  // Increment/decrement local pending requests
  void incrementLocalPendingRequests() { ++local_pending_requests_; }
  void decrementLocalPendingRequests() { --local_pending_requests_; }
  
  // Get host reference for aggregation
  Upstream::HostConstSharedPtr getHost() const { return host_; }
  
  // Get timestamp of last update for aggregation staleness detection
  uint64_t getLastUpdateTimestamp() const { return last_update_timestamp_; }

private:
  Upstream::HostConstSharedPtr host_;
  PeakEwmaCalculator local_ewma_;
  int64_t local_pending_requests_{0};
  uint64_t last_update_timestamp_{0};
};

// Thread-local storage container for all hosts' per-thread statistics
struct PerThreadData : public ThreadLocal::ThreadLocalObject {
public:
  // Map from host to its per-thread statistics
  absl::flat_hash_map<Upstream::HostConstSharedPtr, std::unique_ptr<PerThreadHostStats>> host_stats_;
  
  // Get or create per-thread stats for a host
  PerThreadHostStats& getOrCreateHostStats(Upstream::HostConstSharedPtr host, int64_t tau_nanos);
  
  // Remove host stats when host is removed
  void removeHostStats(Upstream::HostConstSharedPtr host);
};

class PeakEwmaLoadBalancer : public Upstream::ZoneAwareLoadBalancerBase {
public:
  PeakEwmaLoadBalancer(
      const Upstream::PrioritySet& priority_set, const Upstream::PrioritySet* local_priority_set,
      Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
      uint32_t healthy_panic_threshold, const Upstream::ClusterInfo& cluster_info,
      TimeSource& time_source,
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config,
      ThreadLocal::SlotAllocator& tls_allocator);

  Upstream::HostConstSharedPtr chooseHostOnce(Upstream::LoadBalancerContext* context) override;
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext* context) override;

private:
  friend class PeakEwmaTestPeer;

  using HostStatsMap = absl::flat_hash_map<Upstream::HostConstSharedPtr, std::unique_ptr<GlobalHostStats>>;
  using HostCostPair = std::pair<Upstream::HostConstSharedPtr, double>;
  using HostStatIterator = HostStatsMap::iterator;

  Upstream::HostConstSharedPtr selectFromTwoCandidates(
      const Upstream::HostVector& hosts, uint64_t random_value);
  double calculateHostCost(Upstream::HostConstSharedPtr host, HostStatIterator& iterator);
  double calculateHostCostBranchless(double rtt_ewma, double active_requests) const;
  HostStatIterator findHostStats(Upstream::HostConstSharedPtr host);
  
  void onHostSetUpdate(const Upstream::HostVector& hosts_added,
                       const Upstream::HostVector& hosts_removed);
  
  int64_t getCachedTimeNanos() const;
  void prefetchHostData(const Upstream::HostVector& hosts,
                        size_t primary_idx, size_t secondary_idx) const;

  // Thread-local storage access methods
  PerThreadData& getThreadLocalData();
  PerThreadHostStats& getOrCreateThreadLocalHostStats(Upstream::HostConstSharedPtr host);
  void removeThreadLocalHostStats(Upstream::HostConstSharedPtr host);

  const Upstream::ClusterInfo& cluster_info_;
  TimeSource& time_source_;
  const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_proto_;
  const int64_t tau_nanos_;
  Common::CallbackHandlePtr member_update_cb_handle_;
  HostStatsMap host_stats_map_;
  
  // Thread-local storage for per-worker statistics (lazy initialized)
  mutable std::unique_ptr<ThreadLocal::TypedSlot<PerThreadData>> tls_slot_;
  ThreadLocal::SlotAllocator& tls_allocator_;
  
  // Time caching optimization - reduce syscall overhead
  mutable int64_t cached_time_nanos_ = 0;
  mutable uint32_t time_cache_counter_ = 0;
  static constexpr uint32_t kTimeCacheUpdates = 16;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
