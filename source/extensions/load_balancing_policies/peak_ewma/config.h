#pragma once

#include "envoy/upstream/load_balancer.h"
#include "envoy/config/cluster/v3/cluster.pb.h"

#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.validate.h"

#include "source/extensions/load_balancing_policies/common/factory_base.h"


namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

/**
 * Factory for creating Peak EWMA load balancer instances.
 */
class PeakEwmaLoadBalancerFactory : public Upstream::CommonFactoryBase<
                                        envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma> {
public:
  PeakEwmaLoadBalancerFactory() : CommonFactoryBase("envoy.load_balancing_policies.peak_ewma") {}

private:
  Upstream::LoadBalancerPtr create(
      const Upstream::ClusterInfo& cluster, const Upstream::PrioritySet& priority_set,
      const Upstream::HostSet* local_host_set, Runtime::Loader& runtime,
      Random::RandomGenerator& random, TimeSource& time_source,
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config,
      const envoy::config::cluster::v3::Cluster::CommonLbConfig& common_config) const override;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy