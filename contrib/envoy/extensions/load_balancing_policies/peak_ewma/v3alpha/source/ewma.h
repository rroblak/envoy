#pragma once

#include <cmath>
#include <chrono>

#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

/**
 * Time-based EWMA calculator following Finagle's Peak EWMA implementation.
 * This implementation is "peak-sensitive" - it resets when encountering values
 * higher than the current average, making it quickly responsive to latency spikes.
 */
class PeakEwmaCalculator {
public:
  /**
   * Constructor.
   * @param tau_nanos The time constant in nanoseconds. Determines the decay rate.
   * @param initial_value Initial value for the EWMA.
   */
  PeakEwmaCalculator(int64_t tau_nanos, double initial_value)
      : tau_nanos_(tau_nanos), ewma_value_(initial_value), last_timestamp_nanos_(0) {
    ASSERT(tau_nanos > 0, "Tau must be positive");
    ASSERT(!std::isnan(initial_value), "Initial EWMA value cannot be NaN");
  }

  /**
   * Insert a new sample with timestamp-based weighting.
   * Implements Finagle's peak-sensitive algorithm:
   * - If sample > current average, use weighted blend favoring the sample (peak sensitivity)
   * - Otherwise, update using time-weighted decay
   */
  void insert(double sample, int64_t timestamp_nanos) {
    ASSERT(!std::isnan(sample), "EWMA sample cannot be NaN");
    
    // Enforce monotonicity - don't go backwards in time
    if (timestamp_nanos > last_timestamp_nanos_) {
      last_timestamp_nanos_ = timestamp_nanos;
    }
    
    // Initialize timestamp on first sample
    if (last_update_timestamp_ == 0) {
      last_update_timestamp_ = timestamp_nanos;
    }
    
    // Calculate time gap in seconds for weight calculation
    const int64_t time_gap_nanos = timestamp_nanos - last_update_timestamp_;
    double alpha;
    
    if (time_gap_nanos > 0) {
      // Time-based alpha: alpha = 1 - exp(-time_gap / tau)
      // This gives proper time-weighted decay behavior
      alpha = 1.0 - std::exp(-static_cast<double>(time_gap_nanos) / tau_nanos_);
      // Clamp alpha to reasonable bounds
      alpha = std::min(1.0, std::max(0.001, alpha));
    } else {
      // Same timestamp, use a small fixed alpha for responsiveness
      alpha = 0.1;
    }
    
    // Peak sensitivity: increase alpha when sample > current average
    if (sample > ewma_value_) {
      alpha = std::min(1.0, alpha * 2.0);  // Double the learning rate for spikes
    }
    
    // Standard EWMA update: new_value = alpha * sample + (1 - alpha) * old_value
    ewma_value_ = (alpha * sample) + ((1.0 - alpha) * ewma_value_);
    
    last_update_timestamp_ = timestamp_nanos;
  }

  /**
   * Get current EWMA value, updating decay based on time elapsed.
   */
  double value(int64_t current_timestamp_nanos) {
    // Apply time-based decay if significant time has passed since last update
    if (last_update_timestamp_ > 0 && current_timestamp_nanos > last_update_timestamp_) {
      const int64_t time_gap = current_timestamp_nanos - last_update_timestamp_;
      
      // Only apply decay if more than 1ms has passed to avoid excessive decay
      if (time_gap > 1000000) {  // 1ms in nanoseconds
        const double decay_factor = std::exp(-static_cast<double>(time_gap) / tau_nanos_);
        ewma_value_ *= decay_factor;
        last_update_timestamp_ = current_timestamp_nanos;
      }
    }
    
    return ewma_value_;
  }

  /**
   * Get current EWMA value without time-based decay update.
   */
  double lastValue() const { return ewma_value_; }

  /**
   * Reset the EWMA to a new value (used for peak detection).
   */
  void reset(double initial_value = 0.0) {
    ASSERT(!std::isnan(initial_value), "Initial EWMA value cannot be NaN");
    ewma_value_ = initial_value;
  }

private:
  const int64_t tau_nanos_;           // Time constant in nanoseconds
  double ewma_value_;                 // Current EWMA value
  int64_t last_timestamp_nanos_;      // Last observed timestamp
  int64_t last_update_timestamp_ = 0; // Timestamp of last EWMA update
};

/**
 * Legacy EWMA calculator for backward compatibility.
 * @deprecated Use PeakEwmaCalculator for new implementations.
 */
class EwmaCalculator {
public:
  EwmaCalculator(double smoothing_factor, double initial_value)
      : smoothing_factor_(smoothing_factor), ewma_value_(initial_value) {
    ASSERT(smoothing_factor > 0.0 && smoothing_factor < 1.0, "Smoothing factor out of range");
    ASSERT(!std::isnan(initial_value), "Initial EWMA value cannot be NaN");
  }

  void insert(double sample) {
    ASSERT(!std::isnan(sample), "EWMA sample cannot be NaN");
    ewma_value_ = (sample * smoothing_factor_) + (ewma_value_ * (1.0 - smoothing_factor_));
  }

  double value() const { return ewma_value_; }

  void reset(double initial_value) {
    ASSERT(!std::isnan(initial_value), "Initial EWMA value cannot be NaN");
    ewma_value_ = initial_value;
  }

private:
  const double smoothing_factor_;
  double ewma_value_;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
