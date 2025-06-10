#include "source/extensions/load_balancing_policies/peak_ewma/peak_ewma_lb.h"

#include <limits>

#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"
#include "source/common/protobuf/utility.h"
#include "source/common/stats/utility.h" // Add this include for Stats::Utility

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

// Implementation of the PeakEwmaHostStats constructor and methods.
PeakEwmaHostStats::PeakEwmaHostStats(double smoothing_factor, double default_rtt,
                                     TimeSource& /*time_source*/, // time_source is currently unused
                                     Stats::Scope& scope, const Upstream::Host& host)
    : rtt_ewma_(smoothing_factor, default_rtt),
      // FIX: Call gaugeFromElements from the Stats::Utility namespace and add the import mode.
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

Upstream::HostSelectionResponse
PeakEwmaLoadBalancer::chooseHost(Upstream::LoadBalancerContext* /*context*/) {
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

  Upstream::HostConstSharedPtr selected_host = nullptr;
  double min_cost = std::numeric_limits<double>::max();

  for (const auto& host : hosts_to_consider) {
    auto host_stats_opt = host->typedLbPolicyData<PeakEwmaHostStats>();
    ASSERT(host_stats_opt.has_value(), "PeakEwmaHostStats not found on host. Initialization error.");
    if (!host_stats_opt.has_value()) {
      continue;
    }
    PeakEwmaHostStats& host_stats = host_stats_opt.ref();

    double rtt_peak_ewma_ms = host_stats.getEwmaRttMs();
    uint64_t active_requests = host->stats().rq_active_.value();
    double current_cost = rtt_peak_ewma_ms * (static_cast<double>(active_requests) + 1.0);
    host_stats.setComputedCostStat(current_cost);

    if (current_cost < min_cost) {
      min_cost = current_cost;
      selected_host = host;
    } else if (current_cost == min_cost) {
      if (selected_host == nullptr || (random_.random() % 2 == 0)) {
        selected_host = host;
      }
    }
  }

  return Upstream::HostSelectionResponse{selected_host};
}

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::peekAnotherHost(Upstream::LoadBalancerContext* /*context*/) {
  return nullptr;
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
