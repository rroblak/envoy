#pragma once

#include "envoy/upstream/load_balancer.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

// A minimal, self-contained base class for the Peak EWMA load balancer.
class LoadBalancerBase : public Upstream::LoadBalancer {
public:
  LoadBalancerBase(const Upstream::PrioritySet& priority_set,
                   const Upstream::HostSet* local_host_set, Upstream::ClusterLbStats& stats,
                   Runtime::Loader& runtime, Random::RandomGenerator& random)
      : stats_(stats), runtime_(runtime), random_(random), priority_set_(priority_set),
        local_host_set_(local_host_set) {}

  // The chooseHost method must be implemented by the derived class.
  Upstream::HostSelectionResponse chooseHost(Upstream::LoadBalancerContext* context) override = 0;

  // Corrected the return type to match the base class interface.
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext*) override {
    return nullptr;
  }

protected:
  Upstream::ClusterLbStats& stats_;
  Runtime::Loader& runtime_;
  Random::RandomGenerator& random_;
  const Upstream::PrioritySet& priority_set_;
  const Upstream::HostSet* local_host_set_;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
