#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"

#include <limits>

#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"
#include "source/common/common/macros.h"
#include "source/common/protobuf/utility.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

PeakEwmaLoadBalancer::PeakEwmaLoadBalancer(
    const Upstream::LoadBalancerParams& params, const Upstream::ClusterInfo& cluster_info,
    Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
    TimeSource& time_source,
    const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config)
    : PeakEwma::LoadBalancerBase(params.priority_set, params.local_priority_set, stats, runtime,
                                 random),
      cluster_info_(cluster_info), time_source_(time_source), config_proto_(config),
      default_rtt_ms_(
          static_cast<double>(DurationUtil::durationToMilliseconds(config_proto_.default_rtt()))),
      smoothing_factor_(config_proto_.rtt_smoothing_factor()) {

  // Initialize the host stats map for the initial set of hosts.
  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    if (host_set) {
      onHostSetUpdate(host_set->hosts(), {});
    }
  }

  // Set up a callback to update our map when hosts are added or removed.
  member_update_cb_handle_ = priority_set_.addMemberUpdateCb(
      [this](const Upstream::HostVector& hosts_added,
             const Upstream::HostVector& hosts_removed) -> absl::Status {
        onHostSetUpdate(hosts_added, hosts_removed);
        return absl::OkStatus();
      });
}

void PeakEwmaLoadBalancer::onHostSetUpdate(const Upstream::HostVector& hosts_added,
                                           const Upstream::HostVector& hosts_removed) {
  for (const auto& host : hosts_added) {
    // emplace constructs the PeakEwmaHostStats in-place in the map.
    host_stats_map_.emplace(
        host, PeakEwmaHostStats(smoothing_factor_, default_rtt_ms_,
                                cluster_info_.statsScope(), *host));
  }
  for (const auto& host : hosts_removed) {
    host_stats_map_.erase(host);
  }
}

// CORRECTED: This function now accepts a HostConstSharedPtr to be used as a map key.
double PeakEwmaLoadBalancer::getHostCost(const Upstream::HostConstSharedPtr& host) {
  auto it = host_stats_map_.find(host);
  if (it == host_stats_map_.end()) {
    // This should not happen if hosts are correctly managed.
    return std::numeric_limits<double>::max();
  }
  
  PeakEwmaHostStats& stats = it->second;
  double rtt = stats.getEwmaRttMs();
  // Dereference the shared_ptr to get the Host object for stats.
  uint64_t active_requests = host->stats().rq_active_.value();
  double cost = rtt * (static_cast<double>(active_requests) + 1.0);
  stats.setComputedCostStat(cost);
  return cost;
}

Upstream::HostSelectionResponse
PeakEwmaLoadBalancer::chooseHost(Upstream::LoadBalancerContext* context) {
  UNREFERENCED_PARAMETER(context);

  Upstream::HostConstSharedPtr selected_host = nullptr;
  double min_cost = std::numeric_limits<double>::max();

  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    if (host_set) {
      for (const auto& host : host_set->healthyHosts()) {
        // CORRECTED: Pass the HostConstSharedPtr directly.
        const double cost = getHostCost(host);
        if (cost < min_cost) {
          min_cost = cost;
          selected_host = host;
        }
      }
    }
  }

  return {selected_host};
}

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::peekAnotherHost(Upstream::LoadBalancerContext*) {
  // Placeholder implementation.
  return nullptr;
}

PeakEwmaHostStats::PeakEwmaHostStats(double smoothing_factor, double default_rtt,
                                     Stats::Scope& scope, const Upstream::Host& host)
    : rtt_ewma_(smoothing_factor, default_rtt),
      // CORRECTED: Use gaugeFromString, which is the correct method for dynamic stat creation.
      cost_stat_(scope.gaugeFromString(
          absl::StrCat("peak_ewma.", host.address()->asString(), ".cost"),
          Stats::Gauge::ImportMode::NeverImport)) {}

void PeakEwmaHostStats::recordRttSample(std::chrono::milliseconds rtt) {
  rtt_ewma_.insert(static_cast<double>(rtt.count()));
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
