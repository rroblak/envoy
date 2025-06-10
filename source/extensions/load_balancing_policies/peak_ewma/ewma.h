#pragma once

#include <cmath>

#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

/**
 * A simple Exponentially Weighted Moving Average (EWMA) calculator.
 * Formula: EWMA_new = (sample * smoothing_factor) + (EWMA_old * (1 - smoothing_factor))
 */
class EwmaCalculator {
public:
  /**
   * Constructor for EWMA.
   * @param smoothing_factor The weight given to new samples. Must be (0.0, 1.0).
   * @param initial_value The initial value of the EWMA.
   */
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

private:
  const double smoothing_factor_;
  double ewma_value_;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
