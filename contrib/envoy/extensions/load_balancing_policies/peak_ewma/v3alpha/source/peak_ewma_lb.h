#pragma once

#include "envoy/upstream/load_balancer.h"

#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/load_balancer_base.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/ewma.h"

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

  void recordRttSample(std::chrono::milliseconds rtt);

  void setComputedCostStat(double cost) { cost_stat_.set(static_cast<uint64_t>(cost)); }

private:
  EwmaCalculator rtt_ewma_;
  const double default_rtt_ms_;
  Stats::Gauge& cost_stat_;
};

/**
 * This is the implementation of the Peak EWMA load balancer.
 */
class PeakEwmaLoadBalancer : public PeakEwma::LoadBalancerBase {
public:
  // The constructor signature is updated to match the new factory implementation.
  PeakEwmaLoadBalancer(
      const Upstream::LoadBalancerParams& params, const Upstream::ClusterInfo& cluster_info,
      Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
      TimeSource& time_source,
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config);

  // Updated to return HostSelectionResponse to match the interface.
  Upstream::HostSelectionResponse chooseHost(Upstream::LoadBalancerContext* context) override;
  
  // Corrected the return type to match the base class.
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext* context) override;

private:
  friend class PeakEwmaLoadBalancerFactory;

  void initializeHostStats(const Upstream::HostSharedPtr& host);
  void onHostAdded(const Upstream::HostSharedPtr& host);
  double getHostCost(const Upstream::Host& host) const;
  std::tuple<Upstream::HostConstSharedPtr, Upstream::HostConstSharedPtr>
  p2cPick(Upstream::LoadBalancerContext* context, bool peeking);

  TimeSource& time_source_;
  const Upstream::PrioritySet* local_priority_set_{};
  const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_proto_;
  const double default_rtt_ms_;
  const double smoothing_factor_;
  Common::CallbackHandlePtr member_update_cb_handle_;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
