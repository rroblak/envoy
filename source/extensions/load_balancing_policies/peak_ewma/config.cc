#include "source/extensions/load_balancing_policies/peak_ewma/config.h"

#include "source/extensions/load_balancing_policies/peak_ewma/peak_ewma_lb.h"

#include "envoy/upstream/load_balancer.h"
#include "envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

Upstream::LoadBalancerPtr PeakEwmaLoadBalancerFactory::create(
    const Upstream::ClusterInfo& cluster, const Upstream::PrioritySet& priority_set,
    const Upstream::HostSet* local_host_set, Runtime::Loader& runtime,
    Random::RandomGenerator& random, TimeSource& time_source,
    const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config,
    const envoy::config::cluster::v3::Cluster::CommonLbConfig& common_config) const {
  
  // Note: `cluster.lbStats()` provides ClusterLbStats.
  return std::make_unique<PeakEwmaLoadBalancer>(priority_set, local_host_set,
                                                cluster.lbStats(), runtime, random, time_source,
                                                config, common_config);
}

/**
 * Static registration for the Peak EWMA load balancer factory.
 * @return RegisterFactoryContext to register the factory and wrap the stats.
 */
REGISTER_FACTORY(PeakEwmaLoadBalancerFactory, Upstream::TypedLoadBalancerFactory);

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy