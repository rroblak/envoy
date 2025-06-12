#include "source/extensions/load_balancing_policies/peak_ewma/peak_ewma_lb.h"

#include <limits>

#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"
#include "source/common/protobuf/utility.h"
#include "source/common/stats/utility.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

// Implementation of the PeakEwmaHostStats constructor and methods.
PeakEwmaHostStats::PeakEwmaHostStats(double smoothing_factor, double default_rtt,
                                     TimeSource& /*time_source*/, // time_source is currently unused
                                     Stats::Scope& scope, const Upstream::Host& host)
    : rtt_ewma_(smoothing_factor, default_rtt),
      cost_stat_(Stats::Utility::gaugeFromElements(
          scope, {Stats::DynamicName(host.address()->asString()), Stats::DynamicName("peak_ewma_cost")},
          Stats::Gauge::ImportMode::NeverImport)) {}

void PeakEwmaHostStats::recordRttSample(std::chrono::milliseconds rtt) {
  // We insert the RTT value in milliseconds into the EWMA calculator.
  rtt_ewma_.insert(static_cast<double>(rtt.count()));
}

// This constructor now matches the declaration in the updated peak_ewma_lb.h
PeakEwmaLoadBalancer::PeakEwmaLoadBalancer(
    const Upstream::LoadBalancerParams& params, const Upstream::ClusterInfo& cluster_info,
    Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
    TimeSource& time_source,
    const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config)
    : Upstream::LoadBalancerBase(
          params.priority_set, stats, runtime, random,
          PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(cluster_info.lbConfig(),
                                                         healthy_panic_threshold, 100, 50)),
      time_source_(time_source),
      local_priority_set_(params.local_priority_set), random_(random), config_proto_(config),
      default_rtt_ms_(
          static_cast<double>(DurationUtil::durationToMilliseconds(config_proto_.default_rtt()))),
      smoothing_factor_(config_proto_.rtt_smoothing_factor()) {

  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    if (host_set != nullptr) {
      for (const auto& host : host_set->hosts()) {
        initializeHostStats(host);
      }
    }
  }
  if (local_priority_set_ != nullptr) {
    for (const auto& host_set : local_priority_set_->hostSetsPerPriority()) {
      if (host_set != nullptr) {
        for (const auto& host : host_set->hosts()) {
          initializeHostStats(host);
        }
      }
    }
  }

  member_update_cb_handle_ = priority_set_.addMemberUpdateCb(
      [this](const Upstream::HostVector& hosts_added, const Upstream::HostVector&) -> absl::Status {
        for (const auto& host : hosts_added) {
          onHostAdded(host);
        }
        return absl::OkStatus();
      });
}

void PeakEwmaLoadBalancer::initializeHostStats(const Upstream::HostSharedPtr& host) {
  if (!host->typedLbPolicyData<PeakEwmaHostStats>().has_value()) {
    host->setLbPolicyData(std::make_unique<PeakEwmaHostStats>(
        smoothing_factor_, default_rtt_ms_, time_source_, host->cluster().statsScope(), *host));
  }
}

void PeakEwmaLoadBalancer::onHostAdded(const Upstream::HostSharedPtr& host) {
  initializeHostStats(host);
}

// Helper function to compute the cost of a given host.
double PeakEwmaLoadBalancer::getHostCost(const Upstream::Host& host) const {
    auto host_stats_opt = host.typedLbPolicyData<PeakEwmaHostStats>();
    ASSERT(host_stats_opt.has_value(), "PeakEwmaHostStats not found on host. Initialization error.");
    if (!host_stats_opt.has_value()) {
      // Should not happen due to the ASSERT, but return max cost as a fallback.
      return std::numeric_limits<double>::max();
    }
    PeakEwmaHostStats& host_stats = host_stats_opt.ref();

    double rtt_peak_ewma_ms = host_stats.getEwmaRttMs();
    uint64_t active_requests = host.stats().rq_active_.value();
    double current_cost = rtt_peak_ewma_ms * (static_cast<double>(active_requests) + 1.0);
    host_stats.setComputedCostStat(current_cost);

    return current_cost;
}

Upstream::HostSelectionResponse
PeakEwmaLoadBalancer::chooseHost(Upstream::LoadBalancerContext* context) {
  const Upstream::HostSet* current_host_set = nullptr;

  const auto& host_sets = priority_set_.hostSetsPerPriority();
  if (!host_sets.empty()) {
    current_host_set = host_sets[0].get();
  }

  if (local_priority_set_ != nullptr) {
    const auto& local_host_sets = local_priority_set_->hostSetsPerPriority();
    if (!local_host_sets.empty()) {
        current_host_set = local_host_sets[0].get();
    }
  }

  if (!current_host_set) {
    return Upstream::HostSelectionResponse{nullptr};
  }
  
  const auto& hosts_to_consider = current_host_set->healthyHosts();
  if (hosts_to_consider.empty()) {
    return Upstream::HostSelectionResponse{nullptr};
  }

  // If we have only one host, no need for P2C.
  if (hosts_to_consider.size() == 1) {
    // A host is selectable if there is no context or the context doesn't want to select another host.
    const bool h_selectable = !context || !context->shouldSelectAnotherHost(*hosts_to_consider[0]);
    return Upstream::HostSelectionResponse{h_selectable ? hosts_to_consider[0] : nullptr};
  }

  // P2C: Pick two distinct random hosts.
  const uint64_t i1 = random_.random() % hosts_to_consider.size();
  const uint64_t i2 = (i1 + 1 + random_.random() % (hosts_to_consider.size() - 1)) % hosts_to_consider.size();
  Upstream::HostConstSharedPtr h1 = hosts_to_consider[i1];
  Upstream::HostConstSharedPtr h2 = hosts_to_consider[i2];

  // Check if the context allows selection of these hosts. `shouldSelectAnotherHost` returns
  // true if we should *reject* the host, so we negate the result.
  const bool h1_selectable = !context || !context->shouldSelectAnotherHost(*h1);
  const bool h2_selectable = !context || !context->shouldSelectAnotherHost(*h2);

  // Get the cost for each host.
  const double cost1 = getHostCost(*h1);
  const double cost2 = getHostCost(*h2);

  Upstream::HostConstSharedPtr selected_host = nullptr;
  if (h1_selectable && h2_selectable) {
    selected_host = (cost1 < cost2) ? h1 : h2;
  } else if (h1_selectable) {
    selected_host = h1;
  } else if (h2_selectable) {
    selected_host = h2;
  }
  
  // If neither host is selectable, selected_host will be nullptr.
  return Upstream::HostSelectionResponse{selected_host};
}

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::peekAnotherHost(Upstream::LoadBalancerContext* /*context*/) {
  // P2C could also be implemented here, but for now, we leave it as is.
  return nullptr;
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
