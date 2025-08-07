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
    
    // Look for host-attached Peak EWMA atomic ring buffer data
    auto peak_data_opt = host_description->typedLbPolicyData<LoadBalancingPolicies::PeakEwma::PeakEwmaHostLbPolicyData>();
    if (peak_data_opt.has_value()) {
      LoadBalancingPolicies::PeakEwma::PeakEwmaHostLbPolicyData& peak_data = peak_data_opt.ref();
      
      // Calculate TTFB RTT using UpstreamTiming data (more accurate than response time)
      const auto& upstream_timing = upstream_info->upstreamTiming();
      if (upstream_timing.first_upstream_tx_byte_sent_ && 
          upstream_timing.first_upstream_rx_byte_received_) {
        auto ttfb_rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            *upstream_timing.first_upstream_rx_byte_received_ - 
            *upstream_timing.first_upstream_tx_byte_sent_);
        
        // Record RTT sample in host-attached atomic ring buffer (lock-free)
        uint64_t timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        peak_data.recordRttSample(static_cast<double>(ttfb_rtt.count()), timestamp_ns);
        
        // DEBUG: Log RTT sample recording with atomic ring buffer details
        printf("SAMPLE RECORDED: Host=%s RTT=%.1fms Timestamp=%lu (AtomicRingBuffer)\n",
               host_description->address()->asString().c_str(), 
               static_cast<double>(ttfb_rtt.count()),
               timestamp_ns);
        fflush(stdout);
      }
    } else {
      // DEBUG: Log when host doesn't have Peak EWMA data (should not happen after initialization)
      printf("HTTP FILTER WARNING: Host %s missing PeakEwmaHostLbPolicyData\n",
             host_description->address()->asString().c_str());
      fflush(stdout);
    }
  }

  return Http::FilterHeadersStatus::Continue;
}

} // namespace PeakEwma
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
