#pragma once

#include "envoy/event/dispatcher.h"
#include "envoy/upstream/load_balancer.h"
#include "envoy/thread_local/thread_local.h"

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
  PeakEwmaLbConfig(const PeakEwmaLbProto& proto_config, Event::Dispatcher& main_dispatcher,
                   ThreadLocal::SlotAllocator& tls_allocator) 
    : proto_config_(proto_config), main_dispatcher_(main_dispatcher), tls_slot_allocator_(tls_allocator) {
    // Create TLS slot on main thread during config construction
    tls_slot_ = ThreadLocal::TypedSlot<PerThreadData>::makeUnique(tls_allocator);
    tls_slot_->set([](Event::Dispatcher&) -> std::shared_ptr<PerThreadData> {
      return std::make_shared<PerThreadData>();
    });
  }

  const PeakEwmaLbProto proto_config_;
  Event::Dispatcher& main_dispatcher_;
  ThreadLocal::SlotAllocator& tls_slot_allocator_;
  std::unique_ptr<ThreadLocal::TypedSlot<PerThreadData>> tls_slot_;
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
  loadConfig(Server::Configuration::ServerFactoryContext& context,
             const Protobuf::Message& config) override {
    ASSERT(dynamic_cast<const PeakEwmaLbProto*>(&config) != nullptr);
    const PeakEwmaLbProto& typed_config = dynamic_cast<const PeakEwmaLbProto&>(config);
    return Upstream::LoadBalancerConfigPtr{new PeakEwmaLbConfig(typed_config, context.mainThreadDispatcher(), context.threadLocal())};
  }
};

DECLARE_FACTORY(Factory);

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
