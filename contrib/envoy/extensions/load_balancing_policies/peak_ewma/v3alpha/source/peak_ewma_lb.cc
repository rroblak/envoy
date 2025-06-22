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

PeakEwmaHostStats::PeakEwmaHostStats(double smoothing_factor, double default_rtt,
                                     Stats::Scope& scope, const Upstream::Host& host)
    : rtt_ewma_(smoothing_factor, default_rtt),
      cost_stat_(scope.gaugeFromString(
          "peak_ewma." + host.address()->asString() + ".cost",
          Stats::Gauge::ImportMode::NeverImport)) {}

void PeakEwmaHostStats::recordRttSample(std::chrono::milliseconds rtt) {
  rtt_ewma_.insert(static_cast<double>(rtt.count()));
}

PeakEwmaLoadBalancer::PeakEwmaLoadBalancer(
    const Upstream::LoadBalancerParams& params, const Upstream::ClusterInfo& cluster_info,
    Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
    TimeSource& time_source,
    const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config)
    : LoadBalancerBase(params.priority_set, params.local_priority_set, stats, runtime, random),
      cluster_info_(cluster_info),
      time_source_(time_source),
      config_proto_(config),
      default_rtt_ms_(static_cast<double>(
          DurationUtil::durationToMilliseconds(config_proto_.default_rtt()))),
      smoothing_factor_(config_proto_.rtt_smoothing_factor()) {
  member_update_cb_handle_ = params.priority_set.addMemberUpdateCb(
      [this](const Upstream::HostVector& hosts_added, const Upstream::HostVector& hosts_removed) -> absl::Status {
        onHostSetUpdate(hosts_added, hosts_removed);
        return absl::OkStatus();
      });

  for (const auto& host_set : params.priority_set.hostSetsPerPriority()) {
    onHostSetUpdate(host_set->hosts(), {});
  }
}

void PeakEwmaLoadBalancer::onHostSetUpdate(const Upstream::HostVector& hosts_added,
                                           const Upstream::HostVector& hosts_removed) {
  for (const auto& host : hosts_added) {
    host_stats_map_.try_emplace(host, smoothing_factor_, default_rtt_ms_,
                                cluster_info_.statsScope(), *host);
  }
  for (const auto& host : hosts_removed) {
    host_stats_map_.erase(host);
  }
}

double PeakEwmaLoadBalancer::getHostCost(const Upstream::HostConstSharedPtr& host) {
  auto it = host_stats_map_.find(host);
  if (it == host_stats_map_.end()) {
    return std::numeric_limits<double>::max();
  }

  PeakEwmaHostStats& stats = it->second;
  double rtt_ewma = stats.getEwmaRttMs();
  uint64_t active_requests = host->stats().rq_active_.value();
  double cost = rtt_ewma * (static_cast<double>(active_requests) + 1.0);
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

Upstream::HostSelectionResponse
PeakEwmaLoadBalancer::chooseHost(ABSL_ATTRIBUTE_UNUSED Upstream::LoadBalancerContext* context) {
  const auto& host_sets = priority_set_.hostSetsPerPriority();
  const Upstream::HostSet* current_host_set = nullptr;
  for (const auto& host_set : host_sets) {
    if (host_set && !host_set->healthyHosts().empty()) {
      current_host_set = host_set.get();
      break;
    }
  }

  if (current_host_set == nullptr) {
    stats_.lb_healthy_panic_.inc();
    if (!host_sets.empty() && host_sets[0] && !host_sets[0]->hosts().empty()) {
      current_host_set = host_sets[0].get();
    } else {
      return {nullptr};
    }
  }

  const auto& hosts_to_consider = current_host_set->healthyHosts().empty()
                                      ? current_host_set->hosts()
                                      : current_host_set->healthyHosts();

  if (hosts_to_consider.empty()) {
    return {nullptr};
  }

  if (hosts_to_consider.size() == 1) {
    return {hosts_to_consider[0]};
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

  // Optimized: Reduce hash map lookups by batching both cost calculations
  auto it1 = host_stats_map_.find(host1);
  auto it2 = host_stats_map_.find(host2);
  
  const double cost1 = (it1 != host_stats_map_.end()) 
    ? it1->second.getEwmaRttMs() * (static_cast<double>(host1->stats().rq_active_.value()) + 1.0)
    : std::numeric_limits<double>::max();
  
  const double cost2 = (it2 != host_stats_map_.end()) 
    ? it2->second.getEwmaRttMs() * (static_cast<double>(host2->stats().rq_active_.value()) + 1.0)
    : std::numeric_limits<double>::max();
  
  // Update stats if lookups succeeded
  if (it1 != host_stats_map_.end()) {
    it1->second.setComputedCostStat(cost1);
  }
  if (it2 != host_stats_map_.end()) {
    it2->second.setComputedCostStat(cost2);
  }

  Upstream::HostConstSharedPtr selected_host;
  if (cost1 < cost2) {
    selected_host = host1;
  } else if (cost2 < cost1) {
    selected_host = host2;
  } else {
    // Use high bit of random value for tie-breaking instead of new random call
    selected_host = (random_value & 0x8000000000000000ULL) ? host1 : host2;
  }

  return {selected_host};
}

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::peekAnotherHost(ABSL_ATTRIBUTE_UNUSED Upstream::LoadBalancerContext* context) {
  return nullptr;
}

OptRef<Envoy::Http::ConnectionPool::ConnectionLifetimeCallbacks>
PeakEwmaLoadBalancer::lifetimeCallbacks() {
  return {};
}

absl::optional<Upstream::SelectedPoolAndConnection> PeakEwmaLoadBalancer::selectExistingConnection(
    ABSL_ATTRIBUTE_UNUSED Upstream::LoadBalancerContext* context,
    ABSL_ATTRIBUTE_UNUSED const Upstream::Host& host,
    ABSL_ATTRIBUTE_UNUSED std::vector<uint8_t>& hash_key) {
  return absl::nullopt;
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
