#pragma once

#include "envoy/upstream/upstream.h"

#include <cstdint>
#include <utility>

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

/**
 * Peak EWMA cost calculation for host selection.
 * Encapsulates the cost function business logic with zero dependencies.
 */
class Cost {
public:
  /**
   * Constructor with configurable penalty value.
   * @param penalty_value Cost penalty for hosts with no RTT data
   */
  explicit Cost(double penalty_value = 1000000.0) : penalty_value_(penalty_value) {}
  
  /**
   * Compute cost for host selection using Peak EWMA algorithm.
   * Formula: cost = rtt_ewma * (active_requests + 1)
   * 
   * @param rtt_ewma_ms EWMA RTT in milliseconds (0.0 if no data available)
   * @param active_requests Current active request count
   * @param default_rtt_ms Default RTT to use when no EWMA data available
   * @return Computed cost for P2C selection (lower is better)
   */
  double compute(double rtt_ewma_ms, double active_requests, double default_rtt_ms) const;

private:
  const double penalty_value_;
};

/**
 * Power of Two Choices (P2C) host selection algorithm.
 * Selects the better host between two random candidates.
 */
class PowerOfTwoSelector {
public:
  // Mask for tie-breaking using random bits
  static constexpr uint64_t kTieBreakingMask = 0x1;
  
  /**
   * Select the better host between two candidates.
   * 
   * @param first_host First candidate host
   * @param first_cost Cost of first host
   * @param second_host Second candidate host  
   * @param second_cost Cost of second host
   * @param random_value Random value for tie-breaking
   * @return Selected host (lower cost wins, random tie-breaking)
   */
  Upstream::HostConstSharedPtr selectBest(
      Upstream::HostConstSharedPtr first_host, double first_cost,
      Upstream::HostConstSharedPtr second_host, double second_cost,
      uint64_t random_value) const;
  
  /**
   * Generate two distinct indices for P2C selection.
   * 
   * @param host_count Total number of hosts
   * @param random_value Random value for index generation
   * @return Pair of distinct indices
   */
  std::pair<size_t, size_t> generateTwoDistinctIndices(
      size_t host_count, uint64_t random_value) const;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy