#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"

#include <limits>
#include <memory>

#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"
#include "source/common/protobuf/utility.h"

#include "absl/base/attributes.h"
#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

PeakEwmaHostStats::PeakEwmaHostStats(int64_t tau_nanos,
                                     Stats::Scope& scope, const Upstream::Host& host,
                                     TimeSource& time_source)
    : rtt_ewma_(tau_nanos, 0.0),
      time_source_(time_source),
      cost_stat_(scope.gaugeFromString(
          "peak_ewma." + host.address()->asString() + ".cost",
          Stats::Gauge::ImportMode::NeverImport)) {}

double PeakEwmaHostStats::getEwmaRttMs() const {
  // Get current time and update EWMA decay
  int64_t current_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
      time_source_.monotonicTime().time_since_epoch()).count();
  return const_cast<PeakEwmaCalculator&>(rtt_ewma_).value(current_nanos);
}

double PeakEwmaHostStats::getEwmaRttMs(int64_t cached_time_nanos) const {
  // Use pre-computed cached time to avoid syscall overhead
  return const_cast<PeakEwmaCalculator&>(rtt_ewma_).value(cached_time_nanos);
}

void PeakEwmaHostStats::recordRttSample(std::chrono::milliseconds rtt) {
  int64_t timestamp_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
      time_source_.monotonicTime().time_since_epoch()).count();
  rtt_ewma_.insert(static_cast<double>(rtt.count()), timestamp_nanos);
}

PeakEwmaLoadBalancer::PeakEwmaLoadBalancer(
    const Upstream::PrioritySet& priority_set, const Upstream::PrioritySet* local_priority_set,
    Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
    uint32_t healthy_panic_threshold, const Upstream::ClusterInfo& cluster_info,
    TimeSource& time_source,
    const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config)
    : ZoneAwareLoadBalancerBase(priority_set, local_priority_set, stats, runtime, random,
                                healthy_panic_threshold, absl::nullopt),
      cluster_info_(cluster_info),
      time_source_(time_source),
      config_proto_(config),
      tau_nanos_(config_proto_.has_decay_time() ? 
          DurationUtil::durationToMilliseconds(config_proto_.decay_time()) * 1000000LL :
          kDefaultDecayTimeSeconds * 1000000000LL) {
  member_update_cb_handle_ = priority_set.addMemberUpdateCb(
      [this](const Upstream::HostVector& hosts_added, const Upstream::HostVector& hosts_removed) -> absl::Status {
        onHostSetUpdate(hosts_added, hosts_removed);
        return absl::OkStatus();
      });

  for (const auto& host_set : priority_set.hostSetsPerPriority()) {
    onHostSetUpdate(host_set->hosts(), {});
  }
}

void PeakEwmaLoadBalancer::onHostSetUpdate(
    const Upstream::HostVector& hosts_added,
    const Upstream::HostVector& hosts_removed) {
  for (const auto& host : hosts_added) {
    host_stats_map_.try_emplace(host, tau_nanos_,
                                cluster_info_.statsScope(), *host, time_source_);
  }
  for (const auto& host : hosts_removed) {
    host_stats_map_.erase(host);
  }
}

PeakEwmaLoadBalancer::HostStatIterator 
PeakEwmaLoadBalancer::findHostStatsOptimized(Upstream::HostConstSharedPtr host) {
  return host_stats_map_.find(host);
}

double PeakEwmaLoadBalancer::calculateHostCostOptimized(
    Upstream::HostConstSharedPtr host, HostStatIterator& iterator) {
  iterator = findHostStatsOptimized(host);
  if (ABSL_PREDICT_FALSE(iterator == host_stats_map_.end())) {
    return std::numeric_limits<double>::max();
  }

  // Use cached time to reduce syscall overhead
  const int64_t cached_time = getCachedTimeNanos();
  PeakEwmaHostStats& host_stats = iterator->second;
  const double rtt_ewma = host_stats.getEwmaRttMs(cached_time);
  const double active_requests = static_cast<double>(host->stats().rq_active_.value());
  
  const double cost = calculateHostCostBranchless(rtt_ewma, active_requests);
  host_stats.setComputedCostStat(cost);
  return cost;
}

double PeakEwmaLoadBalancer::calculateHostCostBranchless(double rtt_ewma, double active_requests) const {
  const bool has_rtt = (rtt_ewma > 0.0);
  const bool has_requests = (active_requests > 0.0);
  
  // Branchless cost calculation using conditional arithmetic
  const double normal_cost = rtt_ewma * (active_requests + 1.0);
  const double penalty_cost = kPenaltyValue + active_requests;
  const double zero_cost = 0.0;
  
  // Use arithmetic instead of branching for better performance
  return has_rtt ? normal_cost : (has_requests ? penalty_cost : zero_cost);
}

void PeakEwmaLoadBalancer::prefetchHostData(
    const Upstream::HostVector& hosts, size_t start_index) const {
  const size_t host_count = hosts.size();
  const size_t prefetch_end = std::min(start_index + kLoopUnrollFactor, host_count);
  
  for (size_t i = start_index; i < prefetch_end; ++i) {
    __builtin_prefetch(hosts[i].get(), kPrefetchReadHint, kPrefetchHighLocality);
  }
}

std::vector<PeakEwmaLoadBalancer::HostCostPair>
PeakEwmaLoadBalancer::calculateBatchCostsOptimized(const Upstream::HostVector& hosts) {
  std::vector<HostCostPair> results;
  results.reserve(hosts.size());
  
  const size_t host_count = hosts.size();
  const size_t unrolled_end = host_count & ~(kLoopUnrollFactor - 1);
  
  for (size_t i = 0; i < unrolled_end; i += kLoopUnrollFactor) {
    if (i + (kLoopUnrollFactor * 2) < host_count) {
      prefetchHostData(hosts, i + kLoopUnrollFactor);
    }
    
    for (size_t j = 0; j < kLoopUnrollFactor; ++j) {
      const size_t host_index = i + j;
      const auto& host = hosts[host_index];
      
      HostStatIterator iterator;
      const double cost = calculateHostCostOptimized(host, iterator);
      results.emplace_back(host, cost);
    }
  }
  
  for (size_t i = unrolled_end; i < host_count; ++i) {
    const auto& host = hosts[i];
    HostStatIterator iterator;
    const double cost = calculateHostCostOptimized(host, iterator);
    results.emplace_back(host, cost);
  }
  
  return results;
}


Upstream::HostConstSharedPtr PeakEwmaLoadBalancer::selectFromTwoCandidatesOptimized(
    const Upstream::HostVector& hosts, uint64_t random_value) {
  const size_t host_count = hosts.size();
  const size_t first_index = random_value % host_count;
  const size_t second_index = (first_index + 1 + (random_value >> 16) % (host_count - 1)) % host_count;

  const auto& first_host = hosts[first_index];
  const auto& second_host = hosts[second_index];

  // Use intelligent prefetching for the two specific hosts we need
  prefetchHostDataIntelligent(hosts, first_index, second_index);

  HostStatIterator first_iterator;
  HostStatIterator second_iterator;
  
  const double first_cost = calculateHostCostOptimized(first_host, first_iterator);
  const double second_cost = calculateHostCostOptimized(second_host, second_iterator);

  const bool costs_equal = (first_cost == second_cost);
  const bool prefer_first = costs_equal ? 
    (random_value & kTieBreakingMask) != 0 : first_cost < second_cost;
  
  return prefer_first ? first_host : second_host;
}

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::chooseHostOnce(ABSL_ATTRIBUTE_UNUSED Upstream::LoadBalancerContext* context) {
  const auto& host_sets = priority_set_.hostSetsPerPriority();
  const Upstream::HostSet* current_host_set = nullptr;
  
  for (const auto& host_set : host_sets) {
    if (host_set && !host_set->healthyHosts().empty()) {
      current_host_set = host_set.get();
      break;
    }
  }

  if (current_host_set == nullptr) {
    if (!host_sets.empty() && host_sets[0] && !host_sets[0]->hosts().empty()) {
      current_host_set = host_sets[0].get();
    } else {
      return nullptr;
    }
  }

  const auto& hosts_to_consider = current_host_set->healthyHosts().empty()
                                      ? current_host_set->hosts()
                                      : current_host_set->healthyHosts();

  if (hosts_to_consider.empty()) {
    return nullptr;
  }

  if (hosts_to_consider.size() == 1) {
    return hosts_to_consider[0];
  }

  return selectFromTwoCandidatesOptimized(hosts_to_consider, random_.random());
}

int64_t PeakEwmaLoadBalancer::getCachedTimeNanos() const {
  if (++time_cache_counter_ >= kTimeCacheUpdates) {
    cached_time_nanos_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        time_source_.monotonicTime().time_since_epoch()).count();
    time_cache_counter_ = 0;
  }
  return cached_time_nanos_;
}

void PeakEwmaLoadBalancer::prefetchHostDataIntelligent(
    const Upstream::HostVector& hosts, size_t primary_idx, size_t secondary_idx) const {
  // Prefetch the two candidates we'll actually use
  __builtin_prefetch(hosts[primary_idx].get(), kPrefetchReadHint, kPrefetchHighLocality);
  __builtin_prefetch(hosts[secondary_idx].get(), kPrefetchReadHint, kPrefetchHighLocality);
  
  // Prefetch their stats data from the hash map
  auto it1 = host_stats_map_.find(hosts[primary_idx]);
  auto it2 = host_stats_map_.find(hosts[secondary_idx]);
  if (it1 != host_stats_map_.end()) {
    __builtin_prefetch(&it1->second, kPrefetchReadHint, kPrefetchHighLocality);
  }
  if (it2 != host_stats_map_.end()) {
    __builtin_prefetch(&it2->second, kPrefetchReadHint, kPrefetchHighLocality);
  }
}

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::peekAnotherHost(ABSL_ATTRIBUTE_UNUSED Upstream::LoadBalancerContext* context) {
  return nullptr;
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
