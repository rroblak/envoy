#pragma once

#include <cmath>

#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

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
