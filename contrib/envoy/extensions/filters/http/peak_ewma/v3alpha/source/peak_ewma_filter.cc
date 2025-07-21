#include "contrib/envoy/extensions/filters/http/peak_ewma/v3alpha/source/peak_ewma_filter.h"

#include "envoy/stream_info/stream_info.h"
#include "source/common/common/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace PeakEwma {

Http::FilterHeadersStatus PeakEwmaRttFilter::encodeHeaders(Http::ResponseHeaderMap&, bool) {
  // Get upstream host from stream info
  const StreamInfo::StreamInfo& stream_info = encoder_callbacks_->streamInfo();
  const auto& upstream_info = stream_info.upstreamInfo();
  
  if (upstream_info && upstream_info->upstreamHost()) {
    const auto& host_description = upstream_info->upstreamHost();
    auto peak_ewma_stats_opt = host_description->typedLbPolicyData<LoadBalancingPolicies::PeakEwma::GlobalHostStats>();
    if (peak_ewma_stats_opt.has_value()) {
      LoadBalancingPolicies::PeakEwma::GlobalHostStats& stats = peak_ewma_stats_opt.ref();
      
      // Calculate TTFB RTT using UpstreamTiming data
      const auto& upstream_timing = upstream_info->upstreamTiming();
      if (upstream_timing.first_upstream_tx_byte_sent_ && 
          upstream_timing.first_upstream_rx_byte_received_) {
        auto ttfb_rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            *upstream_timing.first_upstream_rx_byte_received_ - 
            *upstream_timing.first_upstream_tx_byte_sent_);
        
        // Record RTT sample for EWMA calculation
        stats.recordRttSample(ttfb_rtt);
      }
    }
  }

  return Http::FilterHeadersStatus::Continue;
}

} // namespace PeakEwma
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
