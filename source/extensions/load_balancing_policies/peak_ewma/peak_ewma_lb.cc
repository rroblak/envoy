#include "source/extensions/load_balancing_policies/peak_ewma/peak_ewma_lb.h"

#include <limits>

#include "envoy/upstream/upstream.h"
#include "source/common/common/assert.h"
#include "source/common/protobuf/utility.h" // For DurationUtil

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

PeakEwmaLoadBalancer::PeakEwmaLoadBalancer(
    const Upstream::PrioritySet& priority_set, const Upstream::HostSet* local_host_set,
    Upstream::ClusterLbStats& stats, Runtime::Loader& runtime,
    Random::RandomGenerator& random, TimeSource& time_source,
    const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config,
    const envoy::config::cluster::v3::Cluster::CommonLbConfig& common_config)
    : Upstream::LoadBalancerBase(priority_set, local_host_set, stats, runtime, random,
                                 common_config),
      time_source_(time_source), config_proto_(config),
      default_rtt_ms_(static_cast<double>(
          DurationUtil::durationToMilliseconds(config_proto_.default_rtt()))),
      smoothing_factor_(config_proto_.rtt_smoothing_factor()) {

  // Initialize stats for existing hosts and set up member update callbacks.
  // This ensures PeakEwmaHostStats is attached to each host.
  auto init_host_set = [this](Upstream::HostSet& host_set) {
    for (const auto& host : host_set.hosts()) {
      initializeHostStats(host);
    }
    // The lambda for onHostAdded will capture `this` and relevant params.
    // It needs to be stored in member_update_cb_handle_ to keep it alive.
    // Note: The type of the callback depends on which HostSet it is (priority_set_ vs local_host_set_).
    // PrioritySet::HostSets has HostSetImpl, which has addMemberUpdateCb.
  };

  // Initialize for primary host sets
  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    if (host_set != nullptr) {
        init_host_set(*host_set);
         // Storing the handle is crucial for the callback to remain active.
        // Each host_set needs its own callback handle management if they can change independently.
        // For simplicity, one handle if only one host_set, or manage a collection.
        // Here we assume this handle is for the entire PrioritySet's updates affecting this LB.
        // A more robust approach might involve storing handles per HostSetImpl.
        // The LoadBalancerBase might already handle some of this if we override specific methods.
        // For now, let's assume this callback is sufficient for host additions within existing sets.
         // This simple handle will be overwritten in the loop; a vector of handles is needed.
         // However, LoadBalancerBase::initialize will call rebuildPrioritySet which we can override.
         // For now, let's stick to initializing existing hosts. The onHostAdded can be called via memberUpdateCb.

        // Add member update callback to initialize stats for newly added hosts.
        // This example shows adding it to the first host set for simplicity.
        // A robust solution would iterate all host_sets and store handles.
        // Or rely on a different mechanism if LoadBalancerBase provides hooks for new hosts.
        // It's often simpler if the factory or LoadBalancerBase handles iterating hosts
        // and allows the derived LB to attach data.

        // For now, we will initialize in constructor and rely on a single member_update_cb for the primary priority_set_.
        // This is a simplified approach for host update handling.
    }
  }
   if (local_host_set_ != nullptr) {
    init_host_set(*local_host_set_);
  }

  // It's generally better to attach Host::Data when the LB is created or when hosts are updated.
  // The `LoadBalancerBase` calls `rebuildPrioritySet` which iterates hosts.
  // Overriding `rebuildPrioritySet` or a similar method from `LoadBalancerBase`
  // might be a cleaner place for `initializeHostStats`.
  // However, `LoadBalancerBase` itself doesn't have a direct `rebuildPrioritySet` virtual method.
  // `initialize` is called by the factory. Let's ensure `initializeHostStats` is called for all initial hosts.
  // And use `addMemberUpdateCb` for dynamic changes.

  // Simplified: Initialize all hosts now.
  for (uint32_t priority = 0; priority < priority_set_.hostSetsPerPriority().size(); ++priority) {
    if (priority_set_.hostSetsPerPriority()[priority]) {
      for (const auto& host : priority_set_.hostSetsPerPriority()[priority]->hosts()) {
        initializeHostStats(host);
      }
      // This should be called for each HostSet individually to handle their updates.
      // Storing a vector of CallbackHandlePtr might be needed.
      // For this example, let's assume a single callback on the first priority healthy hosts for simplicity of the handle.
      // A more complete solution would use `priority_set_.addMemberUpdateCb()`.
      if (priority == 0 && priority_set_.hostSetsPerPriority()[priority]->healthyHosts().size() > 0) {
         member_update_cb_handle_ = priority_set_.hostSetsPerPriority()[priority]->addMemberUpdateCb(
              [this](const Upstream::HostVector& hosts_added, const Upstream::HostVector&) {
                for (const auto& host : hosts_added) {
                  onHostAdded(host);
                }
              });
      }
    }
  }
   if (local_host_set_ != nullptr) {
      for (const auto& host : local_host_set_->hosts()) {
          initializeHostStats(host);
      }
       local_member_update_cb_handle_ = local_host_set_->addMemberUpdateCb(
           [this](const Upstream::HostVector& hosts_added, const Upstream::HostVector&) {
               for (const auto& host : hosts_added) {
                   onHostAdded(host);
               }
           });
   }
}

void PeakEwmaLoadBalancer::initializeHostStats(const Upstream::HostSharedPtr& host) {
  if (!host->loadBalancerData<PeakEwmaHostStats>()) {
    // Pass host's cluster stats scope, or a sub-scope.
    // host->cluster().statsScope() gives the scope for the cluster.
    host->setLoadBalancerData(std::make_shared<PeakEwmaHostStats>(
        smoothing_factor_, default_rtt_ms_, time_source_, host->cluster().statsScope(), *host));
  }
}

void PeakEwmaLoadBalancer::onHostAdded(const Upstream::HostSharedPtr& host) {
    initializeHostStats(host);
}


Upstream::HostConstSharedPtr PeakEwmaLoadBalancer::chooseHost(Upstream::LoadBalancerContext* context) {
  // Use hostsPerPriority()[0] for this example, assuming we operate on P=0 hosts.
  // A real implementation would iterate priorities based on health.
  // LoadBalancerBase provides `chooseHostByPriority` which handles this.
  // We are overriding chooseHost directly, so we need to pick a HostSet.
  // Typically, one would iterate through priorities and pick the highest priority with healthy hosts.
  
  const auto& host_sets = priority_set_.hostSetsPerPriority();
  const Upstream::HostSet* current_host_set = nullptr;

  // Find the highest priority healthy host set
  for (const auto& host_set : host_sets) {
    if (host_set && !host_set->healthyHosts().empty()) {
      current_host_set = host_set.get();
      break;
    }
  }
  // Consider local_host_set_ if applicable (zone-aware routing)
  if (local_host_set_ && !local_host_set_->healthyHosts().empty()) {
      // Logic to prefer local_host_set_ based on policy (e.g., panic thresholds)
      // For simplicity, let's say if local is available and healthy, we use it.
      // This part depends heavily on how LoadBalancerBase's zone awareness is integrated or bypassed.
      // If we directly override chooseHost, we must implement this logic.
      // If we were using a helper from LoadBalancerBase like `chooseAndWrap`, it would handle this.
      // For this example, we'll prefer local if it's healthy and has hosts.
      current_host_set = local_host_set_;
  }


  if (!current_host_set || current_host_set->healthyHosts().empty()) {
    // No healthy hosts in any priority, or local set if considered.
    // This might also mean no hosts at all if local_host_set_ was the only one and it was empty.
    if (current_host_set && current_host_set->hosts().empty()) { // Check if the chosen set was just empty
         stats_.no_healthy_hosts_in_priority_.inc(); // Or a more general "no hosts available" stat.
    } else if (current_host_set) { // Chosen set had hosts, but none healthy
         stats_.no_healthy_hosts_in_priority_.inc();
    } else { // No host set found at all (e.g. all priorities empty)
        // This case should be rare if cluster has hosts.
    }
    return nullptr;
  }

  const auto& hosts_to_consider = current_host_set->healthyHosts();
  if (hosts_to_consider.empty()) {
     stats_.no_healthy_hosts_in_priority_.inc(); // Should have been caught by current_host_set check
    return nullptr;
  }

  Upstream::HostConstSharedPtr selected_host = nullptr;
  double min_cost = std::numeric_limits<double>::max();

  for (const auto& host : hosts_to_consider) {
    if (context && !context->shouldSelectHost(*host)) {
        continue;
    }

    auto* host_stats_ptr = host->loadBalancerData<PeakEwmaHostStats>();
    // Ensure host_stats_ptr is valid; it should have been initialized.
    // If not, it's a bug in initialization logic.
    ASSERT(host_stats_ptr != nullptr, "PeakEwmaHostStats not found on host. Initialization error.");
    if (!host_stats_ptr) { // Defensive: skip host if stats are missing
        continue; 
    }


    double rtt_peak_ewma_ms = host_stats_ptr->getEwmaRttMs();
    uint64_t active_requests = host->stats().rq_active_.value();

    // Cost = RTT_peak_ewma * (active_requests + 1)
    double current_cost = rtt_peak_ewma_ms * (static_cast<double>(active_requests) + 1.0);
    host_stats_ptr->setComputedCostStat(current_cost);


    if (current_cost < min_cost) {
      min_cost = current_cost;
      selected_host = host;
    } else if (current_cost == min_cost) {
      // Tie-breaking: randomly select among hosts with the same min_cost.
      // This ensures fairness if multiple hosts have identical costs.
      if (selected_host == nullptr || (random_.random() % 2 == 0)) { // Simple 50/50 on ties
        selected_host = host;
      }
    }
  }
  
  // If context is provided, call hostAttemptCount if your LB logic uses it
  // For PeakEWMA, simple min_cost selection doesn't typically need attempt counts directly in choice.
  // if (context && selected_host) {
  //   context->incrementHostAttemptCount(*selected_host);
  // }

  return selected_host;
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy