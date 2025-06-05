// source/extensions/load_balancing_policies/peak_ewma/ewma.h
#pragma once

#include <cmath> // For std::isnan

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
   * A lower value means more smoothing and slower response to new samples.
   * For peak-sensitivity, a higher value might be chosen, or the "peak"
   * aspect is handled by how this EWMA is used in the cost function.
   * Your config uses "rtt_smoothing_factor" where a lower value implies
   * more sensitivity to recent changes (which is typical: new_val * alpha + old_val * (1-alpha)).
   * Let's ensure the interpretation matches the formula.
   * If your `rtt_smoothing_factor` is `alpha` in `new*alpha + old*(1-alpha)`, then it's correct.
   * @param initial_value The initial value of the EWMA before any samples are inserted.
   */
  EwmaCalculator(double smoothing_factor, double initial_value)
      : smoothing_factor_(smoothing_factor), ewma_value_(initial_value), initialized_(false) {
    // Smoothing factor should be between 0 and 1 (exclusive for 0, inclusive for 1 is okay for some impls)
    // For PeakEWMA, it's (0.0, 1.0) as per your proto.
    ASSERT(smoothing_factor > 0.0 && smoothing_factor < 1.0, "Smoothing factor out of range");
    ASSERT(!std::isnan(initial_value), "Initial EWMA value cannot be NaN");
  }

  /**
   * Inserts a new sample into the EWMA.
   * @param sample The new sample value.
   */
  void insert(double sample) {
    ASSERT(!std::isnan(sample), "EWMA sample cannot be NaN");
    if (!initialized_) {
      // First sample initializes the EWMA directly if a more complex warmup isn't used.
      // Or, we can just start with the initial_value and apply the formula.
      // Given we have an initial_value, we can consider it "initialized" from the start
      // with that value. Let's assume initial_value is the first "ewma_old".
      // If your initial_value is a default/fallback, then the first sample could directly set ewma_value_.
      // However, using the formula consistently is often simpler.
      ewma_value_ = (sample * smoothing_factor_) + (ewma_value_ * (1.0 - smoothing_factor_));
      initialized_ = true;
    } else {
      ewma_value_ = (sample * smoothing_factor_) + (ewma_value_ * (1.0 - smoothing_factor_));
    }
  }

  /**
   * @return The current EWMA value.
   */
  double value() const { return ewma_value_; }

  /**
   * Resets the EWMA to a new initial value and marks it as uninitialized (or re-initialized).
   * @param initial_value The new initial value.
   */
  void reset(double initial_value) {
    ASSERT(!std::isnan(initial_value), "Initial EWMA value cannot be NaN");
    ewma_value_ = initial_value;
    initialized_ = false; // Or true if `initial_value` means it's immediately ready.
                          // Given the constructor, let's stick to `false` meaning next `insert` will use it as `ewma_old`.
                          // Actually, if we initialize ewma_value_ in constructor, initialized_ can start true.
                          // Let's refine: initialized_ means at least one sample has been processed *or* it's set to a valid start.
                          // The constructor already sets ewma_value_, so it is initialized.
  }

private:
  const double smoothing_factor_;
  double ewma_value_;
  bool initialized_; // Tracks if any samples have been processed or if it's just the initial value.
                     // If constructor sets a valid ewma_value, initialized_ can start as true.
                     // For simplicity, current `insert` logic works even if initialized_ starts true from constructor.
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy