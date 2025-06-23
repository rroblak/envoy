#pragma once

#include "envoy/upstream/load_balancer.h"

#include "source/extensions/load_balancing_policies/common/load_balancer_impl.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/ewma.h"

#include "absl/container/flat_hash_map.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.h"

#include <limits>
#include <vector>

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

constexpr int64_t kDefaultDecayTimeSeconds = 10;
constexpr double kPenaltyValue = static_cast<double>(std::numeric_limits<int64_t>::max() >> 16);
constexpr size_t kCacheLineAlignment = 64;
constexpr size_t kLoopUnrollFactor = 4;
constexpr int kPrefetchReadHint = 0;
constexpr int kPrefetchHighLocality = 3;
constexpr uint64_t kTieBreakingMask = 0x8000000000000000ULL;

namespace {
class PeakEwmaTestPeer;
} // namespace

class PeakEwmaLoadBalancerFactory;

class alignas(kCacheLineAlignment) PeakEwmaHostStats {
public:
  PeakEwmaHostStats(int64_t tau_nanos, Stats::Scope& scope,
                    const Upstream::Host& host, TimeSource& time_source);

  double getEwmaRttMs() const;
  double getEwmaRttMs(int64_t cached_time_nanos) const;
  void recordRttSample(std::chrono::milliseconds rtt);
  void setComputedCostStat(double cost) { cost_stat_.set(static_cast<uint64_t>(cost)); }

  PeakEwmaCalculator rtt_ewma_;

private:
  TimeSource& time_source_;
  Stats::Gauge& cost_stat_;
};

class PeakEwmaLoadBalancer : public Upstream::ZoneAwareLoadBalancerBase {
public:
  PeakEwmaLoadBalancer(
      const Upstream::PrioritySet& priority_set, const Upstream::PrioritySet* local_priority_set,
      Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
      uint32_t healthy_panic_threshold, const Upstream::ClusterInfo& cluster_info,
      TimeSource& time_source,
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config);

  Upstream::HostConstSharedPtr chooseHostOnce(Upstream::LoadBalancerContext* context) override;
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext* context) override;

private:
  friend class PeakEwmaTestPeer;

  using HostStatsMap = absl::flat_hash_map<Upstream::HostConstSharedPtr, PeakEwmaHostStats>;
  using HostCostPair = std::pair<Upstream::HostConstSharedPtr, double>;
  using HostStatIterator = HostStatsMap::iterator;

  Upstream::HostConstSharedPtr selectFromTwoCandidates(
      const Upstream::HostVector& hosts, uint64_t random_value);
  double calculateHostCost(Upstream::HostConstSharedPtr host, HostStatIterator& iterator);
  double calculateHostCostBranchless(double rtt_ewma, double active_requests) const;
  HostStatIterator findHostStats(Upstream::HostConstSharedPtr host);
  std::vector<HostCostPair> calculateBatchCosts(const Upstream::HostVector& hosts);
  void prefetchHostDataBatch(const Upstream::HostVector& hosts, size_t start_index) const;
  
  void onHostSetUpdate(const Upstream::HostVector& hosts_added,
                       const Upstream::HostVector& hosts_removed);
  
  int64_t getCachedTimeNanos() const;
  void prefetchHostData(const Upstream::HostVector& hosts,
                        size_t primary_idx, size_t secondary_idx) const;

  const Upstream::ClusterInfo& cluster_info_;
  TimeSource& time_source_;
  const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_proto_;
  const int64_t tau_nanos_;
  Common::CallbackHandlePtr member_update_cb_handle_;
  HostStatsMap host_stats_map_;
  
  // Time caching optimization - reduce syscall overhead
  mutable int64_t cached_time_nanos_ = 0;
  mutable uint32_t time_cache_counter_ = 0;
  static constexpr uint32_t kTimeCacheUpdates = 16;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
