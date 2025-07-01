#include "contrib/envoy/extensions/filters/http/peak_ewma/v3alpha/source/peak_ewma_filter.h"

#include "envoy/stream_info/stream_info.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace PeakEwma {

void PeakEwmaRttFilter::log(const Http::RequestHeaderMap*, const Http::ResponseHeaderMap*,
                            const Http::ResponseTrailerMap*, const StreamInfo::StreamInfo& info) {
  const auto& upstream_info = info.upstreamInfo();
  if (!upstream_info || !upstream_info->upstreamHost()) {
    return;
  }

  const auto& upstream_timing = upstream_info->upstreamTiming();
  if (!upstream_timing.last_upstream_rx_byte_received_.has_value() ||
      !upstream_timing.first_upstream_tx_byte_sent_.has_value()) {
    return;
  }

  const std::chrono::nanoseconds rtt_ns =
      upstream_timing.last_upstream_rx_byte_received_.value() -
      upstream_timing.first_upstream_tx_byte_sent_.value();

  if (rtt_ns.count() < 0) {
    return;
  }

  const auto& host_description = upstream_info->upstreamHost();
  auto peak_ewma_stats_opt = host_description->typedLbPolicyData<LoadBalancingPolicies::PeakEwma::PeakEwmaHostStats>();
  if (peak_ewma_stats_opt.has_value()) {
    LoadBalancingPolicies::PeakEwma::PeakEwmaHostStats& stats = peak_ewma_stats_opt.ref();
    stats.recordRttSample(
        std::chrono::duration_cast<std::chrono::milliseconds>(rtt_ns));
  }
}

} // namespace PeakEwma
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
