#pragma once

#include "envoy/upstream/load_balancer.h"

#include "source/common/common/logger.h"
#include "source/extensions/load_balancing_policies/common/factory_base.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"

#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.validate.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

using PeakEwmaLbProto = envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma;

class PeakEwmaLbConfig : public Upstream::LoadBalancerConfig {
public:
  PeakEwmaLbConfig(const PeakEwmaLbProto& proto_config) : proto_config_(proto_config) {}

  const PeakEwmaLbProto proto_config_;
};

struct PeakEwmaCreator : public Logger::Loggable<Logger::Id::upstream> {
  Upstream::LoadBalancerPtr operator()(Upstream::LoadBalancerParams params,
                                       OptRef<const Upstream::LoadBalancerConfig> lb_config,
                                       const Upstream::ClusterInfo& cluster_info,
                                       const Upstream::PrioritySet& priority_set,
                                       Runtime::Loader& runtime, Random::RandomGenerator& random,
                                       TimeSource& time_source);
};

class Factory : public Extensions::LoadBalancingPolices::Common::FactoryBase<PeakEwmaLbProto, PeakEwmaCreator> {
public:
  Factory() : FactoryBase("envoy.load_balancing_policies.peak_ewma") {}

  absl::StatusOr<Upstream::LoadBalancerConfigPtr>
  loadConfig(Server::Configuration::ServerFactoryContext&,
             const Protobuf::Message& config) override {
    ASSERT(dynamic_cast<const PeakEwmaLbProto*>(&config) != nullptr);
    const PeakEwmaLbProto& typed_config = dynamic_cast<const PeakEwmaLbProto&>(config);
    return Upstream::LoadBalancerConfigPtr{new PeakEwmaLbConfig(typed_config)};
  }
};

DECLARE_FACTORY(Factory);

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
