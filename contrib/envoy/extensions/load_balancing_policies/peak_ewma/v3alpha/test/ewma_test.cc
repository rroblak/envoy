#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/ewma.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {
namespace {

class EwmaCalculatorTest : public ::testing::Test {
public:
  EwmaCalculatorTest() = default;
};

TEST_F(EwmaCalculatorTest, InitialValue) {
  EwmaCalculator ewma(0.5, 100.0);
  EXPECT_EQ(ewma.value(), 100.0);
}

TEST_F(EwmaCalculatorTest, SingleSample) {
  EwmaCalculator ewma(0.5, 100.0);
  ewma.insert(200.0);
  // Expected: 200 * 0.5 + 100 * 0.5 = 150
  EXPECT_EQ(ewma.value(), 150.0);
}

TEST_F(EwmaCalculatorTest, MultipleSamples) {
  EwmaCalculator ewma(0.5, 100.0);
  
  ewma.insert(200.0);
  EXPECT_EQ(ewma.value(), 150.0);  // 200*0.5 + 100*0.5 = 150
  
  ewma.insert(300.0);
  EXPECT_EQ(ewma.value(), 225.0);  // 300*0.5 + 150*0.5 = 225
  
  ewma.insert(100.0);
  EXPECT_EQ(ewma.value(), 162.5);  // 100*0.5 + 225*0.5 = 162.5
}

TEST_F(EwmaCalculatorTest, DifferentSmoothingFactors) {
  // High smoothing factor (0.9) - more responsive to new samples
  EwmaCalculator ewma_high(0.9, 100.0);
  ewma_high.insert(200.0);
  EXPECT_EQ(ewma_high.value(), 190.0);  // 200*0.9 + 100*0.1 = 190
  
  // Low smoothing factor (0.1) - less responsive to new samples
  EwmaCalculator ewma_low(0.1, 100.0);
  ewma_low.insert(200.0);
  EXPECT_EQ(ewma_low.value(), 110.0);  // 200*0.1 + 100*0.9 = 110
}

TEST_F(EwmaCalculatorTest, ResetFunctionality) {
  EwmaCalculator ewma(0.5, 100.0);
  ewma.insert(200.0);
  EXPECT_EQ(ewma.value(), 150.0);
  
  ewma.reset(300.0);
  EXPECT_EQ(ewma.value(), 300.0);
  
  ewma.insert(100.0);
  EXPECT_EQ(ewma.value(), 200.0);  // 100*0.5 + 300*0.5 = 200
}

TEST_F(EwmaCalculatorTest, ZeroValues) {
  EwmaCalculator ewma(0.5, 0.0);
  EXPECT_EQ(ewma.value(), 0.0);
  
  ewma.insert(100.0);
  EXPECT_EQ(ewma.value(), 50.0);  // 100*0.5 + 0*0.5 = 50
  
  ewma.insert(0.0);
  EXPECT_EQ(ewma.value(), 25.0);  // 0*0.5 + 50*0.5 = 25
}

TEST_F(EwmaCalculatorTest, NegativeValues) {
  EwmaCalculator ewma(0.5, 100.0);
  ewma.insert(-50.0);
  EXPECT_EQ(ewma.value(), 25.0);  // -50*0.5 + 100*0.5 = 25
  
  ewma.insert(-100.0);
  EXPECT_EQ(ewma.value(), -37.5);  // -100*0.5 + 25*0.5 = -37.5
}

TEST_F(EwmaCalculatorTest, VerySmallSmoothingFactor) {
  EwmaCalculator ewma(0.01, 100.0);
  ewma.insert(200.0);
  EXPECT_DOUBLE_EQ(ewma.value(), 101.0);  // 200*0.01 + 100*0.99 = 101
}

TEST_F(EwmaCalculatorTest, VeryLargeSmoothingFactor) {
  EwmaCalculator ewma(0.99, 100.0);
  ewma.insert(200.0);
  EXPECT_DOUBLE_EQ(ewma.value(), 199.0);  // 200*0.99 + 100*0.01 = 199
}

TEST_F(EwmaCalculatorTest, LargeValues) {
  EwmaCalculator ewma(0.5, 1000000.0);
  ewma.insert(2000000.0);
  EXPECT_EQ(ewma.value(), 1500000.0);
}

TEST_F(EwmaCalculatorTest, SmallFractionalValues) {
  EwmaCalculator ewma(0.5, 0.001);
  ewma.insert(0.002);
  EXPECT_DOUBLE_EQ(ewma.value(), 0.0015);  // 0.002*0.5 + 0.001*0.5 = 0.0015
}

// Death tests for invalid inputs
#ifdef NDEBUG
// In release builds, these would not trigger assertions, so skip these tests
TEST_F(EwmaCalculatorTest, DISABLED_InvalidSmoothingFactorZero) {
#else
TEST_F(EwmaCalculatorTest, InvalidSmoothingFactorZero) {
#endif
  EXPECT_DEATH(EwmaCalculator(0.0, 100.0), "Smoothing factor out of range");
}

#ifdef NDEBUG
TEST_F(EwmaCalculatorTest, DISABLED_InvalidSmoothingFactorOne) {
#else
TEST_F(EwmaCalculatorTest, InvalidSmoothingFactorOne) {
#endif
  EXPECT_DEATH(EwmaCalculator(1.0, 100.0), "Smoothing factor out of range");
}

#ifdef NDEBUG
TEST_F(EwmaCalculatorTest, DISABLED_InvalidSmoothingFactorNegative) {
#else
TEST_F(EwmaCalculatorTest, InvalidSmoothingFactorNegative) {
#endif
  EXPECT_DEATH(EwmaCalculator(-0.1, 100.0), "Smoothing factor out of range");
}

#ifdef NDEBUG
TEST_F(EwmaCalculatorTest, DISABLED_InvalidSmoothingFactorTooLarge) {
#else
TEST_F(EwmaCalculatorTest, InvalidSmoothingFactorTooLarge) {
#endif
  EXPECT_DEATH(EwmaCalculator(1.5, 100.0), "Smoothing factor out of range");
}

} // namespace
} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy