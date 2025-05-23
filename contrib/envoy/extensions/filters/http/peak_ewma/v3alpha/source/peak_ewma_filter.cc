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
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterHeadersStatus PeakEwmaRttFilter::encodeHeaders(Http::ResponseHeaderMap&, bool) {
  // Calculate RTT and record it
  const MonotonicTime response_time = encoder_callbacks_->streamInfo().timeSource().monotonicTime();
  const std::chrono::milliseconds rtt = 
      std::chrono::duration_cast<std::chrono::milliseconds>(response_time - request_start_time_);

  // Get upstream host from stream info
  const StreamInfo::StreamInfo& stream_info = encoder_callbacks_->streamInfo();
  const auto& upstream_info = stream_info.upstreamInfo();
  
  if (upstream_info && upstream_info->upstreamHost()) {
    const auto& host_description = upstream_info->upstreamHost();
    auto peak_ewma_stats_opt = host_description->typedLbPolicyData<LoadBalancingPolicies::PeakEwma::PeakEwmaHostStats>();
    if (peak_ewma_stats_opt.has_value()) {
      LoadBalancingPolicies::PeakEwma::PeakEwmaHostStats& stats = peak_ewma_stats_opt.ref();
      stats.recordRttSample(rtt);
    }
  }

  return Http::FilterHeadersStatus::Continue;
}

} // namespace PeakEwma
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
