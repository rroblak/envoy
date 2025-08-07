#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

class CostTest : public ::testing::Test {
protected:
  Cost cost_;
  static constexpr double kDefaultRtt = 10.0;  // 10ms default
};

TEST_F(CostTest, ComputesCostWithRttAndRequests) {
  // RTT available, requests active: cost = rtt * (requests + 1)
  double cost = cost_.compute(20.0, 5.0, kDefaultRtt);
  EXPECT_EQ(cost, 20.0 * (5.0 + 1.0));  // 120.0
}

TEST_F(CostTest, ComputesCostWithRttAndZeroRequests) {
  // RTT available, no requests: cost = rtt * 1
  double cost = cost_.compute(30.0, 0.0, kDefaultRtt);
  EXPECT_EQ(cost, 30.0 * 1.0);  // 30.0
}

TEST_F(CostTest, ComputesCostWithoutRttButWithRequests) {
  // No RTT, but requests active: penalty + requests
  double cost = cost_.compute(0.0, 3.0, kDefaultRtt);
  EXPECT_EQ(cost, Cost::kPenaltyValue + 3.0);
}

TEST_F(CostTest, ComputesCostWithoutRttAndZeroRequests) {
  // No RTT, no requests: use default RTT assumption
  double cost = cost_.compute(0.0, 0.0, kDefaultRtt);
  EXPECT_EQ(cost, kDefaultRtt * 1.0);  // 10.0
}

TEST_F(CostTest, HandlesZeroDefaultRtt) {
  // Edge case: zero default RTT
  double cost = cost_.compute(0.0, 0.0, 0.0);
  EXPECT_EQ(cost, 0.0);
}

TEST_F(CostTest, PrefersFreshRttOverDefault) {
  // When RTT is available, ignore default
  double cost_with_rtt = cost_.compute(50.0, 2.0, kDefaultRtt);
  double expected = 50.0 * (2.0 + 1.0);  // 150.0
  EXPECT_EQ(cost_with_rtt, expected);
  EXPECT_NE(cost_with_rtt, kDefaultRtt * (2.0 + 1.0));  // Should not use default
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy