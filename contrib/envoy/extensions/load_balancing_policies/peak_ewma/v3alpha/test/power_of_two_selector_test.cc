#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"

#include "source/common/network/address_impl.h"
#include "test/mocks/upstream/host.h"

#include "gtest/gtest.h"

using testing::Return;

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

class PowerOfTwoSelectorTest : public ::testing::Test {
protected:
  PowerOfTwoSelector selector_;
  
  // Helper to create a mock host with specific address
  std::shared_ptr<testing::NiceMock<Upstream::MockHost>> createMockHost(const std::string& address) {
    auto host = std::make_shared<testing::NiceMock<Upstream::MockHost>>();
    ON_CALL(*host, address())
        .WillByDefault(Return(std::make_shared<Network::Address::Ipv4Instance>(address, 0)));
    return host;
  }
};

TEST_F(PowerOfTwoSelectorTest, SelectsBetterHostByLowerCost) {
  auto host1 = createMockHost("10.0.0.1");  
  auto host2 = createMockHost("10.0.0.2");
  
  // host1 has higher cost (worse), host2 has lower cost (better)
  auto result = selector_.selectBest(host1, 100.0, host2, 50.0, 0);
  
  EXPECT_EQ(result->address(), host2->address());  // Should select lower cost host
}

TEST_F(PowerOfTwoSelectorTest, SelectsBetterHostByHigherCostWhenReversed) {
  auto host1 = createMockHost("10.0.0.1");  
  auto host2 = createMockHost("10.0.0.2");
  
  // host1 has lower cost (better), host2 has higher cost (worse)
  auto result = selector_.selectBest(host1, 30.0, host2, 80.0, 0);
  
  EXPECT_EQ(result->address(), host1->address());  // Should select lower cost host
}

TEST_F(PowerOfTwoSelectorTest, UsesTieBreakingWhenCostsEqual) {
  auto host1 = createMockHost("10.0.0.1");  
  auto host2 = createMockHost("10.0.0.2");
  
  // Equal costs - decision based on tie breaker
  uint64_t random_prefer_first = PowerOfTwoSelector::kTieBreakingMask;  // bit set
  uint64_t random_prefer_second = 0;  // bit not set
  
  auto result1 = selector_.selectBest(host1, 50.0, host2, 50.0, random_prefer_first);
  auto result2 = selector_.selectBest(host1, 50.0, host2, 50.0, random_prefer_second);
  
  EXPECT_EQ(result1->address(), host1->address());  // Tie breaker prefers first
  EXPECT_EQ(result2->address(), host2->address());  // Tie breaker prefers second
}

TEST_F(PowerOfTwoSelectorTest, GeneratesDistinctIndices) {
  // Test with 3 hosts
  auto [idx1, idx2] = selector_.generateTwoDistinctIndices(3, 0);
  EXPECT_NE(idx1, idx2);  // Must be different
  EXPECT_LT(idx1, 3);     // Must be valid
  EXPECT_LT(idx2, 3);     // Must be valid
}

TEST_F(PowerOfTwoSelectorTest, GeneratesDistinctIndicesForLargerPool) {
  // Test with 10 hosts, various random values
  for (uint64_t random_val : {0UL, 123UL, 456789UL, UINT64_MAX}) {
    auto [idx1, idx2] = selector_.generateTwoDistinctIndices(10, random_val);
    EXPECT_NE(idx1, idx2) << "Failed for random_val=" << random_val;
    EXPECT_LT(idx1, 10) << "Failed for random_val=" << random_val;
    EXPECT_LT(idx2, 10) << "Failed for random_val=" << random_val;
  }
}

TEST_F(PowerOfTwoSelectorTest, HandlesMinimumHostPool) {
  // Edge case: 2 hosts
  auto [idx1, idx2] = selector_.generateTwoDistinctIndices(2, 42);
  EXPECT_NE(idx1, idx2);
  EXPECT_TRUE((idx1 == 0 && idx2 == 1) || (idx1 == 1 && idx2 == 0));
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy