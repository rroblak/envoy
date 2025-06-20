#pragma once

#include <cmath> // For std::isnan

#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

/**
 * A simple Exponentially Weighted Moving Average (EWMA) calculator.
 */
class EwmaCalculator {
public:
  EwmaCalculator(double smoothing_factor, double initial_value)
      : smoothing_factor_(smoothing_factor), ewma_value_(initial_value) {
    ASSERT(smoothing_factor > 0.0 && smoothing_factor < 1.0, "Smoothing factor out of range");
    ASSERT(!std::isnan(initial_value), "Initial EWMA value cannot be NaN");
  }

  /**
   * Inserts a new sample into the EWMA.
   * @param sample The new sample value.
   */
  void insert(double sample) {
    ASSERT(!std::isnan(sample), "EWMA sample cannot be NaN");
    ewma_value_ = (sample * smoothing_factor_) + (ewma_value_ * (1.0 - smoothing_factor_));
  }

  /**
   * @return The current EWMA value.
   */
  double value() const { return ewma_value_; }

  /**
   * Resets the EWMA to a new initial value.
   * @param initial_value The new initial value.
   */
  void reset(double initial_value) {
    ASSERT(!std::isnan(initial_value), "Initial EWMA value cannot be NaN");
    ewma_value_ = initial_value;
  }

private:
  const double smoothing_factor_;
  // CORRECTED: Replaced std::atomic<double> with a plain double, as the LB is
  // worker-local and does not require atomic operations for this value. This makes
  // the class movable and copyable, fixing the build error.
  double ewma_value_;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
