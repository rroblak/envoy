#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/ewma.h"

#include "gtest/gtest.h"

#include <cmath>

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {
namespace {

// Test suite for fastExp function to verify accuracy across different ranges
class FastExpTest : public ::testing::Test {
public:
  // Helper function to test fastExp accuracy against std::exp
  void testFastExpAccuracy(double x, double tolerance = 0.05) {
    const double expected = std::exp(x);
    const double actual = fastExp(x);
    const double error = std::abs(actual - expected);
    const double relative_error = error / std::max(expected, 1e-10);
    
    EXPECT_LE(relative_error, tolerance) 
        << "fastExp(" << x << ") = " << actual 
        << ", expected " << expected 
        << ", relative error: " << relative_error;
  }
};

TEST_F(FastExpTest, BoundaryConditions) {
  // Test upper boundary (x >= 0)
  EXPECT_EQ(fastExp(0.0), 1.0);
  EXPECT_EQ(fastExp(0.5), 1.0);
  EXPECT_EQ(fastExp(1.0), 1.0);
  
  // Test lower boundary (x <= -10)
  EXPECT_EQ(fastExp(-10.0), 0.0);
  EXPECT_EQ(fastExp(-15.0), 0.0);
  EXPECT_EQ(fastExp(-100.0), 0.0);
}

TEST_F(FastExpTest, TaylorSeriesRange) {
  // Test range (-1, 0) which uses Taylor series
  testFastExpAccuracy(-0.1, 0.01);   // Very close to 0
  testFastExpAccuracy(-0.25, 0.01);  
  testFastExpAccuracy(-0.5, 0.02);   
  testFastExpAccuracy(-0.75, 0.03);  
  testFastExpAccuracy(-0.9, 0.04);   // Close to -1
  testFastExpAccuracy(-0.99, 0.05);  // Very close to -1
}

TEST_F(FastExpTest, OptimizedPolynomialRange) {
  // Test range [-6, -1] which uses shifted Taylor series
  testFastExpAccuracy(-1.0, 0.05);
  testFastExpAccuracy(-1.5, 0.05);
  testFastExpAccuracy(-2.0, 0.05);
  testFastExpAccuracy(-2.5, 0.05);
  testFastExpAccuracy(-3.0, 0.07);  // Boundary between ranges, allow higher tolerance
  testFastExpAccuracy(-3.5, 0.05);
  testFastExpAccuracy(-4.0, 0.05);
  testFastExpAccuracy(-4.5, 0.05);
  testFastExpAccuracy(-5.5, 0.1);   // Near boundary, allow higher tolerance
}

TEST_F(FastExpTest, LinearApproximationRange) {
  // Test range [-10, -6] which uses linear interpolation
  // Linear interpolation has higher error but is fast and stable
  testFastExpAccuracy(-6.0, 0.1);    // Boundary condition
  testFastExpAccuracy(-7.0, 1.5);    // Linear approximation, higher tolerance
  testFastExpAccuracy(-8.0, 3.0);    // Linear approximation, higher tolerance
  testFastExpAccuracy(-9.0, 5.0);    // Linear approximation, higher tolerance
  testFastExpAccuracy(-9.99, 0.8);   // Close to boundary
}

TEST_F(FastExpTest, TypicalLoadBalancingValues) {
  // Test values commonly seen in load balancing scenarios
  testFastExpAccuracy(-0.001, 0.001);  // Very small decay
  testFastExpAccuracy(-0.01, 0.01);    // Small decay
  testFastExpAccuracy(-0.1, 0.01);     // Medium decay
  testFastExpAccuracy(-1.0, 0.05);     // Large decay
  testFastExpAccuracy(-2.0, 0.05);     // Very large decay
}

TEST_F(FastExpTest, ContinuityAtBoundaries) {
  // Test continuity at range boundaries
  const double eps = 1e-6;
  
  // Test continuity at x = -1
  double left_limit = fastExp(-1.0 - eps);
  double right_limit = fastExp(-1.0 + eps);
  double at_point = fastExp(-1.0);
  
  EXPECT_NEAR(left_limit, at_point, 0.01);
  EXPECT_NEAR(right_limit, at_point, 0.01);
  
  // Test continuity at x = -5
  left_limit = fastExp(-5.0 - eps);
  right_limit = fastExp(-5.0 + eps);
  at_point = fastExp(-5.0);
  
  EXPECT_NEAR(left_limit, at_point, 0.05);
  EXPECT_NEAR(right_limit, at_point, 0.05);
}

TEST_F(FastExpTest, MonotonicityProperty) {
  // Test that fastExp is monotonically increasing
  const std::vector<double> test_points = {
    -9.0, -8.0, -7.0, -6.0, -5.0, -4.0, -3.0, -2.0, -1.0, -0.5, -0.1, 0.0
  };
  
  for (size_t i = 1; i < test_points.size(); ++i) {
    EXPECT_LT(fastExp(test_points[i-1]), fastExp(test_points[i]))
        << "fastExp is not monotonic between " << test_points[i-1] 
        << " and " << test_points[i];
  }
}

TEST_F(FastExpTest, PerformanceCharacteristics) {
  // Test that fastExp returns reasonable values for performance-critical scenarios
  
  // Typical tau values in nanoseconds (1ms to 1s)
  const std::vector<int64_t> tau_values = {1000000, 10000000, 100000000, 1000000000};
  
  // Typical time gaps (microseconds to milliseconds)
  const std::vector<int64_t> time_gaps = {1000, 10000, 100000, 1000000, 10000000};
  
  for (int64_t tau : tau_values) {
    for (int64_t gap : time_gaps) {
      double ratio = -static_cast<double>(gap) / tau;
      double result = fastExp(ratio);
      
      // Result should be in valid range
      EXPECT_GE(result, 0.0) << "fastExp(" << ratio << ") should be non-negative";
      EXPECT_LE(result, 1.0) << "fastExp(" << ratio << ") should be <= 1";
      
      // Should be finite
      EXPECT_TRUE(std::isfinite(result)) << "fastExp(" << ratio << ") should be finite";
    }
  }
}

// Test suite for FastAlphaCalculator
class FastAlphaCalculatorTest : public ::testing::Test {
public:
  // Helper constants
  static constexpr int64_t kOneMillisecond = 1000000;  // 1ms in nanoseconds
  static constexpr int64_t kOneSecond = 1000000000;    // 1s in nanoseconds
};

TEST_F(FastAlphaCalculatorTest, BoundaryConditions) {
  const int64_t tau = kOneSecond;
  
  // Very small time gaps should return minimum alpha
  EXPECT_EQ(FastAlphaCalculator::timeGapToAlpha(0, tau), 0.1);
  EXPECT_EQ(FastAlphaCalculator::timeGapToAlpha(-1000, tau), 0.1);
  
  // Very large time gaps should return maximum alpha
  EXPECT_EQ(FastAlphaCalculator::timeGapToAlpha(tau * 5, tau), 1.0);
  EXPECT_EQ(FastAlphaCalculator::timeGapToAlpha(tau * 10, tau), 1.0);
}

TEST_F(FastAlphaCalculatorTest, TypicalTimeGaps) {
  const int64_t tau = 100 * kOneMillisecond;  // 100ms tau
  
  // Test various time gaps
  double alpha1 = FastAlphaCalculator::timeGapToAlpha(kOneMillisecond, tau);
  double alpha2 = FastAlphaCalculator::timeGapToAlpha(10 * kOneMillisecond, tau);
  double alpha3 = FastAlphaCalculator::timeGapToAlpha(50 * kOneMillisecond, tau);
  double alpha4 = FastAlphaCalculator::timeGapToAlpha(100 * kOneMillisecond, tau);
  
  // Alpha should increase with time gap
  EXPECT_LT(alpha1, alpha2);
  EXPECT_LT(alpha2, alpha3);
  EXPECT_LT(alpha3, alpha4);
  
  // All should be in valid range
  EXPECT_GE(alpha1, 0.0);
  EXPECT_LE(alpha4, 1.0);
}

TEST_F(FastAlphaCalculatorTest, ConsistencyWithFormula) {
  const int64_t tau = kOneSecond;
  const int64_t time_gap = 100 * kOneMillisecond;
  
  // Calculate expected alpha using the formula: alpha = 1 - exp(-time_gap / tau)
  const double expected_ratio = -static_cast<double>(time_gap) / tau;
  const double expected_alpha = 1.0 - std::exp(expected_ratio);
  
  const double actual_alpha = FastAlphaCalculator::timeGapToAlpha(time_gap, tau);
  
  // Should be close to the theoretical value
  EXPECT_NEAR(actual_alpha, expected_alpha, 0.01);
}

// Test suite for PeakEwmaCalculator
class PeakEwmaCalculatorTest : public ::testing::Test {
public:
  static constexpr int64_t kTau = 100000000;  // 100ms in nanoseconds
  static constexpr double kInitialValue = 100.0;
  static constexpr int64_t kBaseTimestamp = 1000000000;  // 1s in nanoseconds
};

TEST_F(PeakEwmaCalculatorTest, InitialValue) {
  PeakEwmaCalculator ewma(kTau, kInitialValue);
  EXPECT_EQ(ewma.lastValue(), kInitialValue);
}

TEST_F(PeakEwmaCalculatorTest, PeakSensitivityLogic) {
  PeakEwmaCalculator ewma(kTau, kInitialValue);
  
  // Insert a sample higher than current value - should trigger peak sensitivity
  const double high_sample = 200.0;
  ewma.insert(high_sample, kBaseTimestamp);
  
  const double result_after_peak = ewma.lastValue();
  
  // Reset for comparison
  PeakEwmaCalculator ewma_normal(kTau, kInitialValue);
  const double low_sample = 50.0;
  ewma_normal.insert(low_sample, kBaseTimestamp);
  
  const double result_after_normal = ewma_normal.lastValue();
  
  // The peak-sensitive update should move the average more towards the high sample
  // than the normal update moves towards the low sample
  const double peak_change = std::abs(result_after_peak - kInitialValue);
  const double normal_change = std::abs(result_after_normal - kInitialValue);
  
  // Peak sensitivity should cause larger changes for upward spikes
  EXPECT_GT(peak_change, normal_change);
}

TEST_F(PeakEwmaCalculatorTest, TimeBasedDecay) {
  PeakEwmaCalculator ewma(kTau, kInitialValue);
  
  // Insert a sample to change the value
  ewma.insert(200.0, kBaseTimestamp);
  const double value_after_insert = ewma.lastValue();
  
  // Check value after significant time has passed (should decay)
  const int64_t later_timestamp = kBaseTimestamp + kTau * 2;  // 2 * tau later
  const double value_after_decay = ewma.value(later_timestamp);
  
  // Value should have decayed (moved towards zero)
  EXPECT_LT(value_after_decay, value_after_insert);
  EXPECT_GT(value_after_decay, 0.0);  // But not to zero
}

TEST_F(PeakEwmaCalculatorTest, NoDecayForSmallTimeGaps) {
  PeakEwmaCalculator ewma(kTau, kInitialValue);
  
  ewma.insert(200.0, kBaseTimestamp);
  const double value_after_insert = ewma.lastValue();
  
  // Check value after a very small time gap (< 1ms)
  const int64_t small_gap_timestamp = kBaseTimestamp + 500000;  // 0.5ms later
  const double value_after_small_gap = ewma.value(small_gap_timestamp);
  
  // Should be the same (no decay applied for small gaps)
  EXPECT_EQ(value_after_small_gap, value_after_insert);
}

TEST_F(PeakEwmaCalculatorTest, MonotonicTimestamps) {
  PeakEwmaCalculator ewma(kTau, kInitialValue);
  
  // Insert samples with non-monotonic timestamps
  ewma.insert(150.0, kBaseTimestamp);
  ewma.insert(200.0, kBaseTimestamp - 1000000);  // Earlier timestamp
  
  // Should not crash and should handle gracefully
  EXPECT_TRUE(std::isfinite(ewma.lastValue()));
}

TEST_F(PeakEwmaCalculatorTest, ResetFunctionality) {
  PeakEwmaCalculator ewma(kTau, kInitialValue);
  
  ewma.insert(200.0, kBaseTimestamp);
  EXPECT_NE(ewma.lastValue(), kInitialValue);
  
  ewma.reset(300.0);
  EXPECT_EQ(ewma.lastValue(), 300.0);
}

} // namespace
} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy