#include "contrib/envoy/extensions/filters/http/peak_ewma/v3alpha/source/peak_ewma_filter.h"

#include "envoy/stream_info/stream_info.h"
#include "source/common/common/utility.h"
#include "source/common/common/fmt.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace PeakEwma {

PeakEwmaRttFilter::PeakEwmaRttFilter(Stats::ScopeSharedPtr scope)
    : request_start_time_(MonotonicTime()),
      stats_(PeakEwmaFilterStats{ALL_PEAK_EWMA_FILTER_STATS(POOL_COUNTER(*scope), POOL_GAUGE(*scope), POOL_HISTOGRAM(*scope))}),
      scope_(scope) {}


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
  // Calculate RTT and record it - only if we have a valid start time
  if (request_start_time_ == MonotonicTime()) {
    // No start time recorded, skip RTT calculation
    return Http::FilterHeadersStatus::Continue;
  }
  
  const MonotonicTime response_time = encoder_callbacks_->streamInfo().timeSource().monotonicTime();
  const std::chrono::milliseconds rtt = 
      std::chrono::duration_cast<std::chrono::milliseconds>(response_time - request_start_time_);

  // Increment total request counter
  stats_.requests_total_.inc();
  
  // Record global RTT histogram
  stats_.rtt_ms_.recordValue(rtt.count());

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
      
      // Record RTT sample for EWMA calculation
      stats.recordRttSample(rtt);
    }
  }

  return Http::FilterHeadersStatus::Continue;
}

} // namespace PeakEwma
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
