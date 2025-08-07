#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/cost_calculator.h"

#include "envoy/upstream/upstream.h"

#include <cstdio>
#include <string>

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

double CostCalculator::calculateCost(double rtt_ewma_ms, double active_requests, double default_rtt_ms) const {
  const bool has_rtt = (rtt_ewma_ms > 0.0);
  const bool has_requests = (active_requests > 0.0);
  
  if (!has_rtt && has_requests) {
    // Host has requests but no RTT data - likely failing, penalize heavily
    return kPenaltyValue + active_requests;
  } else if (has_rtt) {
    // Standard Peak EWMA formula: cost = latency * load
    return rtt_ewma_ms * (active_requests + 1.0);
  } else {
    // No RTT and no requests: treat as having default RTT performance
    return default_rtt_ms * (active_requests + 1.0);
  }
}

Upstream::HostConstSharedPtr PowerOfTwoSelector::selectBest(
    Upstream::HostConstSharedPtr first_host, double first_cost,
    Upstream::HostConstSharedPtr second_host, double second_cost,
    uint64_t random_value) const {
  const bool costs_equal = (first_cost == second_cost);
  const bool prefer_first = costs_equal ? 
    (random_value & kTieBreakingMask) != 0 : first_cost < second_cost;
  
  // DEBUG: Log unexpected selections where high cost wins
  bool slow_involved = (first_host->address()->asString().find(":19009") != std::string::npos) ||
                       (second_host->address()->asString().find(":19009") != std::string::npos);
  bool slow_selected = prefer_first ? 
    (first_host->address()->asString().find(":19009") != std::string::npos) :
    (second_host->address()->asString().find(":19009") != std::string::npos);
    
  if (slow_involved && slow_selected) {
    printf("SLOW SERVER SELECTED! Cost1=%.1f Cost2=%.1f Equal=%d PreferFirst=%d Random=0x%lx\n",
           first_cost, second_cost, costs_equal, prefer_first, random_value);
    fflush(stdout);
  }
  
  return prefer_first ? first_host : second_host;
}

std::pair<size_t, size_t> PowerOfTwoSelector::generateTwoDistinctIndices(
    size_t host_count, uint64_t random_value) const {
  const size_t first_index = random_value % host_count;
  const size_t second_index = (first_index + 1 + (random_value >> 16) % (host_count - 1)) % host_count;
  return {first_index, second_index};
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy