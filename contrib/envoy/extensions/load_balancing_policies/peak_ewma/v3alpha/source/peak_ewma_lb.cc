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

PeakEwmaHostStats::PeakEwmaHostStats(int64_t tau_nanos, double default_rtt,
                                     Stats::Scope& scope, const Upstream::Host& host,
                                     TimeSource& time_source)
    : rtt_ewma_(tau_nanos, default_rtt),
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
      default_rtt_ms_(static_cast<double>(
          DurationUtil::durationToMilliseconds(config_proto_.default_rtt()))),
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

void PeakEwmaLoadBalancer::onHostSetUpdate(const Upstream::HostVector& hosts_added,
                                           const Upstream::HostVector& hosts_removed) {
  for (const auto& host : hosts_added) {
    host_stats_map_.try_emplace(host, tau_nanos_, default_rtt_ms_,
                                cluster_info_.statsScope(), *host, time_source_);
  }
  for (const auto& host : hosts_removed) {
    host_stats_map_.erase(host);
  }
}

double PeakEwmaLoadBalancer::getHostCost(const Upstream::HostConstSharedPtr& host) {
  auto it = host_stats_map_.find(host);
  if (ABSL_PREDICT_FALSE(it == host_stats_map_.end())) {
    return std::numeric_limits<double>::max();
  }

  // Optimized: Reduce temporary variables and function calls
  PeakEwmaHostStats& stats = it->second;
  const double rtt_ewma = stats.getEwmaRttMs();
  const double cost = rtt_ewma * (static_cast<double>(host->stats().rq_active_.value()) + 1.0);
  stats.setComputedCostStat(cost);
  return cost;
}

std::vector<std::pair<Upstream::HostConstSharedPtr, double>>
PeakEwmaLoadBalancer::calculateBatchCosts(const Upstream::HostVector& hosts) {
  std::vector<std::pair<Upstream::HostConstSharedPtr, double>> results;
  results.reserve(hosts.size());
  
  const size_t host_count = hosts.size();
  
  // Process hosts in groups of 4 for better cache utilization (loop unrolling)
  const size_t unrolled_count = host_count & ~3; // Round down to multiple of 4
  
  for (size_t i = 0; i < unrolled_count; i += 4) {
    // Prefetch next cache lines for better memory access patterns
    if (i + 8 < host_count) {
      // Prefetch host data structures
      __builtin_prefetch(hosts[i + 4].get(), 0, 3);  // Read prefetch, high locality
      __builtin_prefetch(hosts[i + 5].get(), 0, 3);
      __builtin_prefetch(hosts[i + 6].get(), 0, 3);
      __builtin_prefetch(hosts[i + 7].get(), 0, 3);
    }
    
    // Unrolled cost calculations for 4 hosts at once
    for (size_t j = 0; j < 4; ++j) {
      const size_t idx = i + j;
      const auto& host = hosts[idx];
      
      auto it = host_stats_map_.find(host);
      double cost;
      
      if (it != host_stats_map_.end()) {
        const double rtt_ewma = it->second.getEwmaRttMs();
        const uint64_t active_requests = host->stats().rq_active_.value();
        cost = rtt_ewma * (static_cast<double>(active_requests) + 1.0);
        it->second.setComputedCostStat(cost);
      } else {
        cost = std::numeric_limits<double>::max();
      }
      
      results.emplace_back(host, cost);
    }
  }
  
  // Handle remaining hosts (< 4)
  for (size_t i = unrolled_count; i < host_count; ++i) {
    const auto& host = hosts[i];
    auto it = host_stats_map_.find(host);
    double cost;
    
    if (it != host_stats_map_.end()) {
      const double rtt_ewma = it->second.getEwmaRttMs();
      const uint64_t active_requests = host->stats().rq_active_.value();
      cost = rtt_ewma * (static_cast<double>(active_requests) + 1.0);
      it->second.setComputedCostStat(cost);
    } else {
      cost = std::numeric_limits<double>::max();
    }
    
    results.emplace_back(host, cost);
  }
  
  return results;
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

  const size_t host_count = hosts_to_consider.size();
  
  // Optimized P2C: Use single random call for both choices and tie-breaking
  const uint64_t random_value = random_.random();
  const size_t first_choice = random_value % host_count;
  const size_t second_choice = (first_choice + 1 + (random_value >> 16) % (host_count - 1)) % host_count;

  const auto& host1 = hosts_to_consider[first_choice];
  const auto& host2 = hosts_to_consider[second_choice];

  // Memory optimization: Prefetch host data for better cache performance
  __builtin_prefetch(host1.get(), 0, 3);  // Read prefetch, high locality
  __builtin_prefetch(host2.get(), 0, 3);

  // Optimized: Use function to reduce redundant hash lookups and branching
  auto calculateHostCost = [this](const Upstream::HostConstSharedPtr& host) -> std::pair<double, decltype(host_stats_map_.end())> {
    auto it = host_stats_map_.find(host);
    if (it != host_stats_map_.end()) {
      const double rtt = it->second.getEwmaRttMs();
      const double active_requests = static_cast<double>(host->stats().rq_active_.value());
      const double cost = rtt * (active_requests + 1.0);
      return {cost, it};
    }
    return {std::numeric_limits<double>::max(), it};
  };
  
  // Calculate costs with single hash lookup per host
  auto [cost1, it1] = calculateHostCost(host1);
  auto [cost2, it2] = calculateHostCost(host2);
  
  // Update stats if lookups succeeded (branchless when possible)
  if (it1 != host_stats_map_.end()) {
    it1->second.setComputedCostStat(cost1);
  }
  if (it2 != host_stats_map_.end()) {
    it2->second.setComputedCostStat(cost2);
  }

  // Optimized: Branchless host selection using conditional moves
  // For equal costs, use tie-breaking; otherwise select lower cost
  const bool costs_equal = (cost1 == cost2);
  const bool prefer_host1 = costs_equal ? 
    (random_value & 0x8000000000000000ULL) != 0 : cost1 < cost2;
  
  return prefer_host1 ? host1 : host2;
}

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::peekAnotherHost(ABSL_ATTRIBUTE_UNUSED Upstream::LoadBalancerContext* context) {
  return nullptr;
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
