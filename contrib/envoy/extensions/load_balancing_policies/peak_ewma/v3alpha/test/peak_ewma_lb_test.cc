/*
 * FILE PATH: test/extensions/load_balancing_policies/peak_ewma/peak_ewma_lb_test.cc
 */

#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.h"

#include "source/common/upstream/upstream_impl.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/ewma.h"
#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"

#include "test/mocks/common.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {
namespace {

// Test the EwmaCalculator helper class to ensure the math is correct.
TEST(EwmaCalculatorTest, BasicOperation) {
  // 20% weight for new samples, 100ms initial value.
  EwmaCalculator calc(0.2, 100.0);
  EXPECT_DOUBLE_EQ(calc.value(), 100.0);

  // Insert a new sample of 50ms.
  // Expected calculation: (50.0 * 0.2) + (100.0 * 0.8) = 10 + 80 = 90
  calc.insert(50.0);
  EXPECT_DOUBLE_EQ(calc.value(), 90.0);

  // Insert a new sample of 150ms.
  // Expected calculation: (150.0 * 0.2) + (90.0 * 0.8) = 30 + 72 = 102
  calc.insert(150.0);
  EXPECT_DOUBLE_EQ(calc.value(), 102.0);
}

// Test fixture to set up the PeakEwmaLoadBalancer and its mocks.
class PeakEwmaLoadBalancerTest : public ::testing::Test {
public:
  PeakEwmaLoadBalancerTest() {
    // Create 3 mock hosts and add them to the host set.
    for (int i = 0; i < 3; ++i) {
      hosts_.emplace_back(std::make_shared<NiceMock<Upstream::MockHost>>());
      // Ensure rq_active is available on each host's stats.
      ON_CALL(*hosts_[i], stats()).WillByDefault(ReturnRef(host_stats_[i]));
    }
    host_set_.hosts_ = hosts_;
    host_set_.healthy_hosts_ = hosts_;

    // The priority set contains our single host set.
    priority_set_.host_sets_ = {&host_set_};
  }

  // Initializes the load balancer with a given YAML configuration.
  void initialize(const std::string& yaml_config) {
    // Parse the YAML config into the protobuf message.
    TestUtility::loadFromYaml(yaml_config, config_);

    // Create the load balancer instance with all its mocked dependencies.
    lb_ = std::make_unique<PeakEwmaLoadBalancer>(priority_set_, nullptr, stats_, runtime_, random_,
                                                time_source_, config_, common_config_);
  }

  // Helper to set a host's active requests and EWMA RTT for testing.
  void setHostStats(size_t host_index, uint64_t active_requests, double ewma_rtt) {
    // Set the active request count on the host's stats object.
    host_stats_[host_index].rq_active_.set(active_requests);

    // To set the EWMA, we access the HostLbPolicyData attached by the LB.
    // This simulates the state of the host for a predictable test.
    auto* peak_ewma_stats = hosts_[host_index]->loadBalancerData<PeakEwmaHostStats>();
    ASSERT_NE(peak_ewma_stats, nullptr) << "Host " << host_index << " was not initialized by the LB.";

    // Manually reset the internal EWMA value.
    peak_ewma_stats->ewma_rtt_calculator_.reset(ewma_rtt);
  }

protected:
  envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_;
  envoy::config::cluster::v3::Cluster::CommonLbConfig common_config_;
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  Upstream::HostSetImpl host_set_{0, 0};
  std::vector<std::shared_ptr<Upstream::MockHost>> hosts_;

  // We need real stats objects for each host to mock rq_active.
  std::array<Upstream::HostStats, 3> host_stats_;

  std::shared_ptr<NiceMock<Upstream::MockClusterInfo>> cluster_info_{
      std::make_shared<NiceMock<Upstream::MockClusterInfo>>()};
  NiceMock<Upstream::MockClusterStats> stats_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Random::MockRandomGenerator> random_;
  NiceMock<MockTimeSystem> time_source_;

  std::unique_ptr<PeakEwmaLoadBalancer> lb_;
};

// Test that the host with the absolute lowest cost is chosen.
// Cost = RTT * (active_requests + 1)
TEST_F(PeakEwmaLoadBalancerTest, SelectsHostWithLowestCost) {
  const std::string yaml = R"EOF(
    default_rtt: 100ms
    rtt_smoothing_factor: 0.5
  )EOF";
  initialize(yaml);

  // Setup host stats and calculate expected costs:
  // Host 0: Cost = 20ms * (1 + 1) = 40
  setHostStats(0, 1, 20.0);
  // Host 1: Cost = 10ms * (2 + 1) = 30  <- MINIMUM COST
  setHostStats(1, 2, 10.0);
  // Host 2: Cost = 5ms * (10 + 1) = 55
  setHostStats(2, 10, 5.0);

  // We expect Host 1 to be chosen as it has the lowest cost.
  EXPECT_EQ(lb_->chooseHost(nullptr).get(), hosts_[1].get());
}

// Test that if costs are equal, we tie-break randomly.
TEST_F(PeakEwmaLoadBalancerTest, TieBreaking) {
  const std::string yaml = R"EOF(
    default_rtt: 100ms
    rtt_smoothing_factor: 0.5
  )EOF";
  initialize(yaml);

  // Setup two hosts with identical costs.
  // Host 0: Cost = 20ms * (4 + 1) = 100
  setHostStats(0, 4, 20.0);
  // Host 1: Cost = 10ms * (9 + 1) = 100
  setHostStats(1, 9, 10.0);
  // Host 2: Cost = 50ms * (5 + 1) = 300 (high cost, should not be chosen)
  setHostStats(2, 5, 50.0);

  // With two hosts at the same minimum cost, we should pick one of them.
  // We can mock the random generator to force a choice for a deterministic test.
  EXPECT_CALL(random_, random()).WillOnce(Return(0)); // Mock random to return 0
  EXPECT_EQ(lb_->chooseHost(nullptr).get(), hosts_[0].get());

  EXPECT_CALL(random_, random()).WillOnce(Return(1)); // Mock random to return 1
  EXPECT_EQ(lb_->chooseHost(nullptr).get(), hosts_[1].get());
}

// Test the RTT update mechanism to ensure the feedback loop works.
TEST_F(PeakEwmaLoadBalancerTest, RttUpdate) {
  const std::string yaml = R"EOF(
    default_rtt: 100ms
    rtt_smoothing_factor: 0.2
  )EOF";
  initialize(yaml);

  // Get the stats object for host 0, which should have been attached during LB init.
  auto* stats = hosts_[0]->loadBalancerData<PeakEwmaHostStats>();
  ASSERT_NE(stats, nullptr);

  // Initial value should be the default RTT from the config.
  EXPECT_DOUBLE_EQ(stats->getEwmaRttMs(), 100.0);

  // Report a new RTT measurement. This happens via the HostLbPolicyData interface.
  stats->onHostRttReported(std::chrono::milliseconds(50));

  // Verify the EWMA calculation is correct.
  // Expected: (50 * 0.2) + (100 * 0.8) = 10 + 80 = 90
  EXPECT_DOUBLE_EQ(stats->getEwmaRttMs(), 90.0);
}

} // namespace
} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
