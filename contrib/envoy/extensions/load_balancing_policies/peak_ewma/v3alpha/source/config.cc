#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/config.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

Upstream::LoadBalancerPtr PeakEwmaCreator::operator()(
    Upstream::LoadBalancerParams /* params */, OptRef<const Upstream::LoadBalancerConfig> lb_config,
    const Upstream::ClusterInfo& cluster_info, const Upstream::PrioritySet& priority_set,
    Runtime::Loader& runtime, Envoy::Random::RandomGenerator& random, TimeSource& time_source) {
  
  const auto* config = dynamic_cast<const TypedPeakEwmaLbConfig*>(lb_config.ptr());
  if (config == nullptr) {
    ENVOY_LOG(error, "Peak EWMA load balancer config is required");
    return nullptr;
  }

  return std::make_unique<PeakEwmaLoadBalancer>(
      priority_set, nullptr, cluster_info.lbStats(), runtime, random,
      50, // healthy_panic_threshold
      cluster_info, time_source, config->lb_config_, 
      config->main_dispatcher_);
}

/**
 * Static registration for the Factory. @see RegisterFactory.
 */
REGISTER_FACTORY(Factory, Upstream::TypedLoadBalancerFactory);

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
