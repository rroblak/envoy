#pragma once

#include "envoy/upstream/load_balancer.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"
#include "source/common/common/logger.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

class LoadBalancerBase : public Upstream::LoadBalancer, 
                         public Logger::Loggable<Logger::Id::upstream> {
public:
  LoadBalancerBase(const Upstream::PrioritySet& priority_set,
                   const Upstream::PrioritySet* local_priority_set,
                   Upstream::ClusterLbStats& stats, Runtime::Loader& runtime,
                   Random::RandomGenerator& random)
      : stats_(stats), runtime_(runtime), random_(random),
        priority_set_(priority_set), local_priority_set_(local_priority_set) {}

  Upstream::HostSelectionResponse chooseHost(Upstream::LoadBalancerContext* context) override = 0;

  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext*) override {
    return nullptr;
  }

protected:
  Upstream::ClusterLbStats& stats_;
  Runtime::Loader& runtime_;
  Random::RandomGenerator& random_;
  const Upstream::PrioritySet& priority_set_;
  const Upstream::PrioritySet* local_priority_set_;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy