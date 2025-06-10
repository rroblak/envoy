#pragma once

#include "envoy/upstream/load_balancer.h"

#include "source/extensions/load_balancing_policies/common/load_balancer_impl.h"
#include "source/extensions/load_balancing_policies/peak_ewma/ewma.h"

#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

// Forward declaration of the factory
class PeakEwmaLoadBalancerFactory;

/**
 * This is a custom LB policy data structure that will be attached to each host.
 * It stores the EWMA latency for the host.
 */
class PeakEwmaHostStats : public Upstream::HostLbPolicyData {
public:
  PeakEwmaHostStats(double smoothing_factor, double default_rtt, TimeSource& time_source,
                    Stats::Scope& scope, const Upstream::Host& host);

  double getEwmaRttMs() const { return rtt_ewma_.value(); }

  // FIX: The method is renamed to match the call site in peak_ewma_filter.cc
  void recordRttSample(std::chrono::milliseconds rtt);

  void setComputedCostStat(double cost) { cost_stat_.set(static_cast<uint64_t>(cost)); }

private:
  EwmaCalculator rtt_ewma_;
  Stats::Gauge& cost_stat_;
};

/**
 * This is the implementation of the Peak EWMA load balancer.
 */
class PeakEwmaLoadBalancer : public Upstream::LoadBalancerBase {
public:
  // The constructor signature is updated to match the implementation.
  PeakEwmaLoadBalancer(
      const Upstream::LoadBalancerParams& params,
      // Dependencies like stats, runtime, etc., are passed directly from the factory
      // instead of being accessed from a common_config struct.
      const Upstream::ClusterInfo& cluster_info, Upstream::ClusterLbStats& stats,
      Runtime::Loader& runtime, Random::RandomGenerator& random, TimeSource& time_source,
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config);

  // Upstream::LoadBalancer
  Upstream::HostSelectionResponse chooseHost(Upstream::LoadBalancerContext* context) override;
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext* context) override;

private:
  friend class PeakEwmaLoadBalancerFactory;

  void initializeHostStats(const Upstream::HostSharedPtr& host);
  void onHostAdded(const Upstream::HostSharedPtr& host);

  TimeSource& time_source_;
  // This member is added to hold the local priority set.
  const Upstream::PrioritySet* local_priority_set_{};
  Random::RandomGenerator& random_;
  const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_proto_;
  const double default_rtt_ms_;
  const double smoothing_factor_;
  // The type for the callback handle is corrected to Common::CallbackHandlePtr
  // based on the return type of PrioritySet::addMemberUpdateCb.
  Common::CallbackHandlePtr member_update_cb_handle_;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
