#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/config.h"

#include "envoy/registry/registry.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

class LbFactory : public Upstream::LoadBalancerFactory {
public:
  LbFactory(const PeakEwmaLbConfig& config, const Upstream::ClusterInfo& cluster_info,
            Runtime::Loader& runtime, Random::RandomGenerator& random, TimeSource& time_source)
      : config_(config.proto_config_), cluster_info_(cluster_info), runtime_(runtime),
        random_(random), time_source_(time_source) {}

  Upstream::LoadBalancerPtr create(Upstream::LoadBalancerParams params) override {
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

class PeakEwmaThreadAwareLb : public Upstream::ThreadAwareLoadBalancer {
public:
  PeakEwmaThreadAwareLb(Upstream::LoadBalancerFactorySharedPtr factory)
      : factory_(std::move(factory)) {}

  Upstream::LoadBalancerFactorySharedPtr factory() override { return factory_; }

  absl::Status initialize() override { return absl::OkStatus(); }

private:
  Upstream::LoadBalancerFactorySharedPtr factory_;
};


Upstream::ThreadAwareLoadBalancerPtr PeakEwmaLoadBalancerFactory::create(
    OptRef<const Upstream::LoadBalancerConfig> lb_config,
    const Upstream::ClusterInfo& cluster_info, const Upstream::PrioritySet&,
    Runtime::Loader& runtime, Random::RandomGenerator& random, TimeSource& time_source) {

  const auto* config = dynamic_cast<const PeakEwmaLbConfig*>(lb_config.ptr());
  ASSERT(config != nullptr, "Invalid config passed to PeakEwmaLoadBalancerFactory::create");

  auto factory = std::make_shared<LbFactory>(*config, cluster_info, runtime, random, time_source);
  return std::make_unique<PeakEwmaThreadAwareLb>(std::move(factory));
}

absl::StatusOr<Upstream::LoadBalancerConfigPtr> PeakEwmaLoadBalancerFactory::loadConfig(
    Server::Configuration::ServerFactoryContext&, const Protobuf::Message& config) {
  const auto& typed_config = dynamic_cast<
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma&>(config);
  return std::make_unique<PeakEwmaLbConfig>(typed_config);
}

REGISTER_FACTORY(PeakEwmaLoadBalancerFactory, Upstream::TypedLoadBalancerFactory);

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
