#pragma once

#include "envoy/http/filter.h"
#include "envoy/upstream/cluster_manager.h"

#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "source/extensions/load_balancing_policies/peak_ewma/peak_ewma_lb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

/**
 * This filter's purpose is to observe stream completion and record the upstream
 * request RTT into the PeakEwmaHostStats for the host that was used.
 * This is necessary because the load balancer itself does not have direct access
 * to the final RTT of a request it initiated.
 */
class PeakEwmaRttFilter : public Http::PassThroughFilter {
public:
  // Http::StreamFilterBase
  // The 'log' method in the base class is not virtual, so 'override' is removed.
  void log(const Http::RequestHeaderMap*, const Http::ResponseHeaderMap*,
           const Http::ResponseTrailerMap*, const StreamInfo::StreamInfo& info);
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
