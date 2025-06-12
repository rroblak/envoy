#include "source/extensions/load_balancing_policies/peak_ewma/peak_ewma_lb.h"

#include <limits>
#include <tuple>

#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.hh"
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
    : rtt_ewma_(smoothing_factor, default_rtt), default_rtt_ms_(default_rtt),
      cost_stat_(Stats::Utility::gaugeFromElements(
          scope, {Stats::DynamicName(host.address()->asString()), Stats::DynamicName("peak_ewma_cost")},
          Stats::Gauge::ImportMode::NeverImport)) {}

// CONCURRENCY NOTE: This method is a "check-then-act" operation that is not fully
// atomic, but is still thread-safe and correct in this context.
//
// The underlying `rtt_ewma_` uses `std::atomic`, so all individual reads (`.value()`)
// and writes (`.reset()`, `.insert()`) are atomic, preventing data corruption.
//
// A race condition can occur where one thread reads the current EWMA, a second
// thread updates it, and then the first thread acts on the old value. This is
// acceptable because we want to be pessimistic. If any thread observes a high
// latency "peak", we want its subsequent `reset()` to take precedence over any
// concurrent `insert()` operations that might be processing a "good" latency
// sample. This ensures the load balancer reacts aggressively to signs of an
// unhealthy host.
void PeakEwmaHostStats::recordRttSample(std::chrono::milliseconds rtt) {
  const double rtt_ms = static_cast<double>(rtt.count());
  // This is the "peak" check. If the new RTT is significantly higher than the
  // current average, we suspect an anomaly and reset the EWMA to the default.
  // This allows the LB to react quickly to a host that has suddenly become slow.
  if (rtt_ms > rtt_ewma_.value()) {
    rtt_ewma_.reset(default_rtt_ms_);
  } else {
    rtt_ewma_.insert(rtt_ms);
  }
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
    // Pass the default RTT from the main LB config to the host-local stats object.
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

// Helper function to perform the P2C host selection.
std::tuple<Upstream::HostConstSharedPtr, Upstream::HostConstSharedPtr>
PeakEwmaLoadBalancer::p2cPick(Upstream::LoadBalancerContext* context, bool peeking) {
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
    return {nullptr, nullptr};
  }
 
  const auto& hosts_to_consider = current_host_set->healthyHosts();
  if (hosts_to_consider.size() < 2) {
      if (hosts_to_consider.size() == 1 && !peeking) {
          // In the case of a single host, we can still "choose" it. Peeking makes no sense.
          const bool h_selectable = !context || !context->shouldSelectAnotherHost(*hosts_to_consider[0]);
          return {h_selectable ? hosts_to_consider[0] : nullptr, nullptr};
      }
      return {nullptr, nullptr};
  }

  // P2C: Pick two distinct random hosts.
  const uint64_t i1 = random_.random() % hosts_to_consider.size();
  const uint64_t i2 = (i1 + 1 + random_.random() % (hosts_to_consider.size() - 1)) % hosts_to_consider.size();
 
  return {hosts_to_consider[i1], hosts_to_consider[i2]};
}

Upstream::HostSelectionResponse
PeakEwmaLoadBalancer::chooseHost(Upstream::LoadBalancerContext* context) {
  auto [h1, h2] = p2cPick(context, false);

  if (h1 == nullptr) {
      // This can happen if there are no hosts, or only one host which was not selectable.
      // If there was one host and it was selectable, it would have been returned by p2cPick.
      return Upstream::HostSelectionResponse{nullptr};
  }
 
  if (h2 == nullptr) {
      // This case means there was only one host and it was selectable.
      return Upstream::HostSelectionResponse{h1};
  }

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
PeakEwmaLoadBalancer::peekAnotherHost(Upstream::LoadBalancerContext* context) {
  auto [h1, h2] = p2cPick(context, true);

  if (h1 == nullptr || h2 == nullptr) {
      // Peeking requires two hosts to choose between.
      return nullptr;
  }

  // Check if the context allows selection of these hosts.
  const bool h1_selectable = !context || !context->shouldSelectAnotherHost(*h1);
  const bool h2_selectable = !context || !context->shouldSelectAnotherHost(*h2);

  // Get the cost for each host.
  const double cost1 = getHostCost(*h1);
  const double cost2 = getHostCost(*h2);

  // The peeked host should be the host that would *not* have been chosen by chooseHost.
  // This provides a logical alternative for pre-connection.
  if (h1_selectable && h2_selectable) {
    return (cost1 < cost2) ? h2 : h1;
  }

  // If only one or neither of the hosts is selectable, there's no viable "other" host to peek.
  return nullptr;
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
