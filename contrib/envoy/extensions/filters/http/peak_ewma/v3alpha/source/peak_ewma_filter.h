#pragma once

#include "envoy/http/filter.h"
#include "envoy/upstream/cluster_manager.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"

#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace PeakEwma {

/**
 * All peak ewma filter stats. @see stats_macros.h
 */
#define ALL_PEAK_EWMA_FILTER_STATS(COUNTER, GAUGE, HISTOGRAM)                                     \
  COUNTER(requests_total)                                                                          \
  HISTOGRAM(rtt_ms, Milliseconds)

/**
 * Struct definition for all peak ewma filter stats. @see stats_macros.h
 */
struct PeakEwmaFilterStats {
  ALL_PEAK_EWMA_FILTER_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT,
                              GENERATE_HISTOGRAM_STRUCT)
};

class PeakEwmaRttFilter : public Http::PassThroughFilter {
public:
  PeakEwmaRttFilter(Stats::ScopeSharedPtr scope);
  
  // Override decode headers to start timing
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers, bool end_stream) override;
  
  // Override encode headers to capture RTT
  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers, bool end_stream) override;

private:
  MonotonicTime request_start_time_;
  PeakEwmaFilterStats stats_;
  Stats::ScopeSharedPtr scope_;
};

} // namespace PeakEwma
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
