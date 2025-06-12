#pragma once

#include <atomic>
#include <cmath>

#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

/**
 * A simple Exponentially Weighted Moving Average (EWMA) calculator.
 * Formula: EWMA_new = (sample * smoothing_factor) + (EWMA_old * (1 - smoothing_factor))
 * This implementation is thread-safe using std::atomic.
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
   * Inserts a new sample into the EWMA in a thread-safe manner.
   * @param sample The new sample value.
   */
  void insert(double sample) {
    ASSERT(!std::isnan(sample), "EWMA sample cannot be NaN");
    // Use a compare-exchange loop for a thread-safe read-modify-write operation.
    // This is the correct pattern for updating an atomic value based on its previous state.
    double old_val = ewma_value_.load(std::memory_order_relaxed);
    double new_val;
    do {
      new_val = (sample * smoothing_factor_) + (old_val * (1.0 - smoothing_factor_));
    } while (!ewma_value_.compare_exchange_weak(old_val, new_val, std::memory_order_release,
                                               std::memory_order_relaxed));
  }

  /**
   * Resets the EWMA to a specific value.
   * @param value The value to reset to.
   */
  void reset(double value) {
    ASSERT(!std::isnan(value), "EWMA reset value cannot be NaN");
    ewma_value_.store(value, std::memory_order_release);
  }

  /**
   * @return The current EWMA value, read atomically.
   */
  double value() const {
    // A relaxed memory order is sufficient as we only need atomicity for this read,
    // not synchronization with other memory operations.
    return ewma_value_.load(std::memory_order_relaxed);
  }

private:
  const double smoothing_factor_;
  // The EWMA value is now an atomic double to prevent data races between worker threads.
  std::atomic<double> ewma_value_;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
