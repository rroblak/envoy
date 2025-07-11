#include "contrib/envoy/extensions/filters/http/peak_ewma/v3alpha/source/peak_ewma_filter.h"

#include "envoy/stream_info/stream_info.h"
#include "source/common/common/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace PeakEwma {

Http::FilterHeadersStatus PeakEwmaRttFilter::decodeHeaders(Http::RequestHeaderMap&, bool) {
  // Record request start time
  request_start_time_ = decoder_callbacks_->streamInfo().timeSource().monotonicTime();
  
  // Increment pending requests for the upstream host (when known)
  const StreamInfo::StreamInfo& stream_info = decoder_callbacks_->streamInfo();
  const auto& upstream_info = stream_info.upstreamInfo();
  
  if (upstream_info && upstream_info->upstreamHost()) {
    const auto& host_description = upstream_info->upstreamHost();
    auto peak_ewma_stats_opt = host_description->typedLbPolicyData<LoadBalancingPolicies::PeakEwma::GlobalHostStats>();
    if (peak_ewma_stats_opt.has_value()) {
      LoadBalancingPolicies::PeakEwma::GlobalHostStats& stats = peak_ewma_stats_opt.ref();
      stats.incrementPendingRequests();
    }
  }
  
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterHeadersStatus PeakEwmaRttFilter::encodeHeaders(Http::ResponseHeaderMap&, bool) {
  // TODO: Calculate RTT and record it in Phase 3
  // const MonotonicTime response_time = encoder_callbacks_->streamInfo().timeSource().monotonicTime();
  // const std::chrono::milliseconds rtt = 
  //     std::chrono::duration_cast<std::chrono::milliseconds>(response_time - request_start_time_);

  // Get upstream host from stream info
  const StreamInfo::StreamInfo& stream_info = encoder_callbacks_->streamInfo();
  const auto& upstream_info = stream_info.upstreamInfo();
  
  if (upstream_info && upstream_info->upstreamHost()) {
    const auto& host_description = upstream_info->upstreamHost();
    auto peak_ewma_stats_opt = host_description->typedLbPolicyData<LoadBalancingPolicies::PeakEwma::GlobalHostStats>();
    if (peak_ewma_stats_opt.has_value()) {
      LoadBalancingPolicies::PeakEwma::GlobalHostStats& stats = peak_ewma_stats_opt.ref();
      // Decrement pending requests when request completes
      stats.decrementPendingRequests();
      
      // TODO: RTT recording will be handled by per-thread stats in Phase 3
      // For now, we'll skip RTT recording to avoid breaking the atomic model
    }
  }

  return Http::FilterHeadersStatus::Continue;
}

} // namespace PeakEwma
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
