#pragma once

#include "envoy/upstream/load_balancer.h"

#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"

#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.validate.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

/**
 * A simple configuration class to hold the parsed proto config.
 * This will be passed from the main thread to worker threads.
 */
class PeakEwmaLbConfig : public Upstream::LoadBalancerConfig {
public:
  PeakEwmaLbConfig(const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma&
                       proto_config)
      : proto_config_(proto_config) {}

  const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma proto_config_;
};

/**
 * Factory for creating Peak EWMA load balancer instances.
 * This class now directly implements the TypedLoadBalancerFactory interface.
 */
class PeakEwmaLoadBalancerFactory : public Upstream::TypedLoadBalancerFactory {
public:
  std::string name() const override { return "envoy.load_balancing_policies.peak_ewma"; }

  // Creates the ThreadAwareLoadBalancer.
  Upstream::ThreadAwareLoadBalancerPtr
  create(OptRef<const Upstream::LoadBalancerConfig> lb_config,
         const Upstream::ClusterInfo& cluster_info, const Upstream::PrioritySet& priority_set,
         Runtime::Loader& runtime, Random::RandomGenerator& random,
         TimeSource& time_source) override;

  // FIX: The signature of loadConfig is corrected to match the base class.
  // The first parameter is the context, the second is the config message.
  absl::StatusOr<Upstream::LoadBalancerConfigPtr>
  loadConfig(Server::Configuration::ServerFactoryContext& context,
             const Protobuf::Message& config) override;

  // FIX: Added missing pure virtual method implementation.
  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<
        envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma>();
  }
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
