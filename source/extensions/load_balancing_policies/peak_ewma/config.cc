#include "source/extensions/load_balancing_policies/peak_ewma/config.h"

#include "envoy/registry/registry.h"
#include "source/extensions/load_balancing_policies/common/load_balancer_impl.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

/**
 * This is the actual factory that creates a PeakEwmaLoadBalancer on each worker thread.
 * It's created once on the main thread and shared with all worker threads.
 */
class LbFactory : public Upstream::LoadBalancerFactory {
public:
  LbFactory(const PeakEwmaLbConfig& config, const Upstream::ClusterInfo& cluster_info,
            Runtime::Loader& runtime, Random::RandomGenerator& random, TimeSource& time_source)
      : config_(config.proto_config_), cluster_info_(cluster_info), runtime_(runtime),
        random_(random), time_source_(time_source) {}

  Upstream::LoadBalancerPtr create(Upstream::LoadBalancerParams params) override {
    // This is where the actual load balancer instance is created on a worker thread.
    // It correctly calls the PeakEwmaLoadBalancer constructor with all necessary dependencies.
    return std::make_unique<PeakEwmaLoadBalancer>(params, cluster_info_, cluster_info_.lbStats(),
                                                  runtime_, random_, time_source_, config_);
  }

private:
  const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_;
  const Upstream::ClusterInfo& cluster_info_;
  Runtime::Loader& runtime_;
  Random::RandomGenerator& random_;
  TimeSource& time_source_;
};

/**
 * This is the implementation for the Thread-Aware Load Balancer wrapper.
 * Its main job is to hold the LbFactory.
 */
class ThreadAwareLb : public Upstream::ThreadAwareLoadBalancer {
public:
  ThreadAwareLb(Upstream::LoadBalancerFactorySharedPtr factory) : factory_(std::move(factory)) {}
  Upstream::LoadBalancerFactorySharedPtr factory() override { return factory_; }
  absl::Status initialize() override { return absl::OkStatus(); }

private:
  Upstream::LoadBalancerFactorySharedPtr factory_;
};

// Implementation of the main factory's create() method.
Upstream::ThreadAwareLoadBalancerPtr PeakEwmaLoadBalancerFactory::create(
    OptRef<const Upstream::LoadBalancerConfig> lb_config,
    const Upstream::ClusterInfo& cluster_info, const Upstream::PrioritySet&,
    Runtime::Loader& runtime, Random::RandomGenerator& random, TimeSource& time_source) {

  const auto* config = dynamic_cast<const PeakEwmaLbConfig*>(lb_config.ptr());
  ASSERT(config != nullptr, "Invalid config passed to PeakEwmaLoadBalancerFactory::create");

  // Create the LbFactory that will be shared across worker threads.
  auto factory = std::make_shared<LbFactory>(*config, cluster_info, runtime, random, time_source);
  // Return the thread-aware wrapper.
  return std::make_unique<ThreadAwareLb>(std::move(factory));
}

// Implementation of the main factory's loadConfig() method.
absl::StatusOr<Upstream::LoadBalancerConfigPtr> PeakEwmaLoadBalancerFactory::loadConfig(
    Server::Configuration::ServerFactoryContext&, const Protobuf::Message& config) {
  // Cast the generic proto message to our specific type and wrap it in our config object.
  const auto& typed_config = dynamic_cast<
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma&>(config);
  return std::make_unique<PeakEwmaLbConfig>(typed_config);
}

// Static registration for the Peak EWMA load balancer factory.
REGISTER_FACTORY(PeakEwmaLoadBalancerFactory, Upstream::TypedLoadBalancerFactory);

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
