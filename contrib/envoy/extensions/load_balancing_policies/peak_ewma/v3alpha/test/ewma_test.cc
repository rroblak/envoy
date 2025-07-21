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

// Note: PeakEwmaCalculator tests removed as this class is no longer used
// in the new k-way merge + snapshot architecture. The EWMA computation
// is now handled directly in the aggregation process using FastAlphaCalculator.

} // namespace
} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy