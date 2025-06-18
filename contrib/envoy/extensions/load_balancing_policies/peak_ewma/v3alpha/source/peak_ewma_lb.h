#pragma once

#include "envoy/upstream/load_balancer.h"

#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/load_balancer_base.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/ewma.h"

#include "absl/container/flat_hash_map.h"
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
class PeakEwmaHostStats {
public:
  // Corrected constructor signature.
  PeakEwmaHostStats(double smoothing_factor, double default_rtt, Stats::Scope& scope,
                    const Upstream::Host& host);

  double getEwmaRttMs() const { return rtt_ewma_.value(); }
  void recordRttSample(std::chrono::milliseconds rtt);
  void setComputedCostStat(double cost) { cost_stat_.set(static_cast<uint64_t>(cost)); }

private:
  EwmaCalculator rtt_ewma_;
  // The gauge is now stored directly, not as a reference.
  Stats::Gauge& cost_stat_;
};

/**
 * This is the implementation of the Peak EWMA load balancer.
 */
class PeakEwmaLoadBalancer : public PeakEwma::LoadBalancerBase {
public:
  PeakEwmaLoadBalancer(
      const Upstream::LoadBalancerParams& params, const Upstream::ClusterInfo& cluster_info,
      Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
      TimeSource& time_source,
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config);

  Upstream::HostSelectionResponse chooseHost(Upstream::LoadBalancerContext* context) override;
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext* context) override;

private:
  friend class PeakEwmaLoadBalancerFactory;

  // This map will hold the stats for each host, removing the need to modify the Host object.
  using HostStatsMap = absl::flat_hash_map<Upstream::HostConstSharedPtr, PeakEwmaHostStats>;

  void onHostSetUpdate(const Upstream::HostVector& hosts_added,
                       const Upstream::HostVector& hosts_removed);
  
  // CORRECTED: The signature now matches the implementation in the .cc file.
  double getHostCost(const Upstream::HostConstSharedPtr& host);

  const Upstream::ClusterInfo& cluster_info_;
  TimeSource& time_source_;
  const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_proto_;
  const double default_rtt_ms_;
  const double smoothing_factor_;
  Common::CallbackHandlePtr member_update_cb_handle_;
  HostStatsMap host_stats_map_;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
