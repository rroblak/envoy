#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"
#include "envoy/config/cluster/v3/cluster.pb.h"

#include "source/common/common/logger.h"
#include "source/common/network/address_impl.h"

#include "test/common/stats/stat_test_utility.h"
#include "test/mocks/common.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/test_common/status_utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <chrono>
#include <iostream>

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::ReturnRef;
using testing::SaveArg;

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {
namespace {

class PeakEwmaTestPeer {
public:
  PeakEwmaTestPeer(PeakEwmaLoadBalancer& lb) : lb_(lb) {}
  absl::flat_hash_map<Upstream::HostConstSharedPtr, PeakEwmaHostStats>& hostStatsMap() {
    return lb_.host_stats_map_;
  }
private:
  PeakEwmaLoadBalancer& lb_;
};

class PeakEwmaLoadBalancerTest : public ::testing::Test {
public:
  PeakEwmaLoadBalancerTest()
      : stat_names_(store_.symbolTable()),
        stats_(stat_names_, *store_.rootScope()) {

    // Create 3 mock hosts
    for (int i = 0; i < 3; ++i) {
      auto mock_host = std::make_shared<NiceMock<Upstream::MockHost>>();
      ON_CALL(*mock_host, address())
          .WillByDefault(Return(
              std::make_shared<Network::Address::Ipv4Instance>("10.0.0." + std::to_string(i + 1))));
      // MockHost already provides a working stats() method, no need to mock it
      hosts_.emplace_back(mock_host);
    }
  }

  void SetUp() override {
    // Use getMockHostSet(0) for priority 0 host set
    host_set_ = priority_set_.getMockHostSet(0);

    // Set up the hosts in the host set
    host_set_->hosts_ = hosts_;
    host_set_->healthy_hosts_ = hosts_;

    ON_CALL(priority_set_, hostSetsPerPriority()).WillByDefault(ReturnRef(priority_set_.host_sets_));
    ON_CALL(*cluster_info_, statsScope()).WillByDefault(ReturnRef(*store_.rootScope()));

    // Set up time source mock - return incremental time values
    current_time_ = std::chrono::nanoseconds(1000000000); // Start at 1 second
    EXPECT_CALL(time_source_, monotonicTime()).WillRepeatedly(Invoke([this]() {
      current_time_ += std::chrono::microseconds(100); // Increment by 100μs each call
      return MonotonicTime(current_time_);
    }));

    Upstream::LoadBalancerParams params{priority_set_, nullptr};
    config_.mutable_decay_time()->set_seconds(10);

    lb_ = std::make_unique<PeakEwmaLoadBalancer>(
        params.priority_set, params.local_priority_set, stats_, runtime_, random_, 50,
        *cluster_info_, time_source_, config_);

    // Trigger the member update callback by calling runCallbacks
    host_set_->runCallbacks(hosts_, {});
  }

  void setHostStats(Upstream::HostConstSharedPtr host, std::chrono::milliseconds rtt,
                    uint32_t active_requests) {
    host->stats().rq_active_.set(active_requests);

    // Use internal map for tests (mock hosts don't support setLbPolicyData)
    PeakEwmaTestPeer peer(*lb_);
    auto& map = peer.hostStatsMap();
    auto it = map.find(host);
    ASSERT_NE(it, map.end()) << "Host " << host->address()->asString() << " not found in host_stats_map_";
    it->second.rtt_ewma_.reset(static_cast<double>(rtt.count()));
  }

  Stats::TestUtil::TestStore store_;
  Upstream::ClusterLbStatNames stat_names_;
  Upstream::ClusterLbStats stats_;

  std::vector<Upstream::HostSharedPtr> hosts_;
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  Upstream::MockHostSet* host_set_{nullptr};
  std::shared_ptr<NiceMock<Upstream::MockClusterInfo>> cluster_info_{
      std::make_shared<NiceMock<Upstream::MockClusterInfo>>()};
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Random::MockRandomGenerator> random_;
  MockTimeSystem time_source_;
  std::chrono::nanoseconds current_time_;
  envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_;
  std::unique_ptr<PeakEwmaLoadBalancer> lb_;
};

TEST_F(PeakEwmaLoadBalancerTest, P2CSelectsHostWithLowerCost) {
  setHostStats(hosts_[0], std::chrono::milliseconds(100), 1);  // cost = 100 * 2 = 200
  setHostStats(hosts_[1], std::chrono::milliseconds(20), 2);   // cost = 20 * 3 = 60
  setHostStats(hosts_[2], std::chrono::milliseconds(50), 10);  // cost = 50 * 11 = 550

  // Force P2C to select hosts_[0] and hosts_[1] using optimized single random call
  // random_value = 0: first_choice = 0 % 3 = 0, second_choice = (0 + 1 + 0) % 3 = 1
  EXPECT_CALL(random_, random()).WillOnce(Return(0));

  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[1] because it has lower cost (60 < 200)
  EXPECT_EQ(result.host, hosts_[1]);
}

TEST_F(PeakEwmaLoadBalancerTest, P2CTieBreaking) {
  setHostStats(hosts_[0], std::chrono::milliseconds(50), 3);  // cost = 50 * 4 = 200
  setHostStats(hosts_[1], std::chrono::milliseconds(40), 4);  // cost = 40 * 5 = 200
  setHostStats(hosts_[2], std::chrono::milliseconds(100), 5); // cost = 100 * 6 = 600

  // Force P2C to select hosts_[0] and hosts_[1] (both have same cost)
  // random_value = 0: high bit is 0, so selects host2 (hosts_[1])
  EXPECT_CALL(random_, random()).WillOnce(Return(0));

  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[1] because high bit is 0
  EXPECT_EQ(result.host, hosts_[1]);

  // Test the other tie-breaking case
  // random_value with high bit set: selects host1 (hosts_[0])
  EXPECT_CALL(random_, random()).WillOnce(Return(0x8000000000000000ULL));

  result = lb_->chooseHost(nullptr);
  // Should select hosts_[0] because high bit is 1
  EXPECT_EQ(result.host, hosts_[0]);
}

TEST_F(PeakEwmaLoadBalancerTest, P2CSingleHost) {
  // Remove all but one host
  host_set_->hosts_ = {hosts_[0]};
  host_set_->healthy_hosts_ = {hosts_[0]};
  host_set_->runCallbacks({hosts_[0]}, {hosts_[1], hosts_[2]});

  setHostStats(hosts_[0], std::chrono::milliseconds(100), 1);

  auto result = lb_->chooseHost(nullptr);
  // Should select the only host without calling random
  EXPECT_EQ(result.host, hosts_[0]);
}

TEST_F(PeakEwmaLoadBalancerTest, NoHealthyHosts) {
  // Set up hosts but no healthy hosts
  host_set_->hosts_ = hosts_;
  host_set_->healthy_hosts_ = {};
  host_set_->runCallbacks({}, {});

  setHostStats(hosts_[0], std::chrono::milliseconds(100), 1);
  setHostStats(hosts_[1], std::chrono::milliseconds(200), 2);
  setHostStats(hosts_[2], std::chrono::milliseconds(300), 3);

  // Should fall back to all hosts and use P2C among them
  // random_value = 0: first_choice = 0, second_choice = 1
  EXPECT_CALL(random_, random()).WillOnce(Return(0));

  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[0] because it has lower cost (200 < 400)
  EXPECT_EQ(result.host, hosts_[0]);
}

TEST_F(PeakEwmaLoadBalancerTest, NoHostsAvailable) {
  // Remove all hosts
  host_set_->hosts_ = {};
  host_set_->healthy_hosts_ = {};
  host_set_->runCallbacks({}, {hosts_[0], hosts_[1], hosts_[2]});

  auto result = lb_->chooseHost(nullptr);
  // Should return nullptr when no hosts available
  EXPECT_EQ(result.host, nullptr);
}

TEST_F(PeakEwmaLoadBalancerTest, HostStatsUpdate) {
  setHostStats(hosts_[0], std::chrono::milliseconds(100), 5);

  // Verify host stats are properly stored and can be retrieved via internal map
  PeakEwmaTestPeer peer(*lb_);
  auto& map = peer.hostStatsMap();
  auto it = map.find(hosts_[0]);
  ASSERT_NE(it, map.end());

  // Check that EWMA value was set correctly (account for time decay)
  EXPECT_NEAR(it->second.getEwmaRttMs(), 100.0, 10.0);  // Allow 10ms tolerance for time decay

  // Update stats and verify EWMA updates
  it->second.recordRttSample(std::chrono::milliseconds(200));
  // With time-based EWMA: peak sensitivity increases learning rate for spikes
  // Expected: weighted blend between 100 and 200, closer to 200 due to peak sensitivity
  EXPECT_GT(it->second.getEwmaRttMs(), 100.0);  // Should be greater than original
  EXPECT_LT(it->second.getEwmaRttMs(), 200.0);  // But not a full reset to 200
}

TEST_F(PeakEwmaLoadBalancerTest, CostCalculation) {
  // Test various cost scenarios
  setHostStats(hosts_[0], std::chrono::milliseconds(50), 0);   // cost = 50 * 1 = 50
  setHostStats(hosts_[1], std::chrono::milliseconds(100), 1);  // cost = 100 * 2 = 200
  setHostStats(hosts_[2], std::chrono::milliseconds(25), 3);   // cost = 25 * 4 = 100

  // Force selection of hosts_[0] vs hosts_[2]
  // random_value = 65539: first_choice = 0, second_choice = 2
  EXPECT_CALL(random_, random()).WillOnce(Return(65539));

  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[0] because it has lower cost (50 < 100)
  EXPECT_EQ(result.host, hosts_[0]);
}

TEST_F(PeakEwmaLoadBalancerTest, P2CRandomSelectionMocked) {
  // Set all hosts to have same cost for fair comparison
  setHostStats(hosts_[0], std::chrono::milliseconds(100), 1);  // cost = 200
  setHostStats(hosts_[1], std::chrono::milliseconds(100), 1);  // cost = 200
  setHostStats(hosts_[2], std::chrono::milliseconds(100), 1);  // cost = 200

  // Test first combination: hosts_[0] vs hosts_[1], should select one of them
  // random_value = 0: first_choice = 0, second_choice = 1
  EXPECT_CALL(random_, random()).WillOnce(Return(0));

  auto result1 = lb_->chooseHost(nullptr);
  // Should select either hosts_[0] or hosts_[1] - both are valid with similar costs
  EXPECT_TRUE(result1.host == hosts_[0] || result1.host == hosts_[1]);

  // Test second combination: different random value should still select a valid host
  // random_value = 0x8000000000010001ULL gives: first_choice=1, second_choice=0, high_bit=1
  EXPECT_CALL(random_, random()).WillOnce(Return(0x8000000000010001ULL));

  auto result2 = lb_->chooseHost(nullptr);
  // Should select either hosts_[0] or hosts_[1] - both are valid with similar costs
  EXPECT_TRUE(result2.host == hosts_[0] || result2.host == hosts_[1]);
}

TEST_F(PeakEwmaLoadBalancerTest, HealthyPanicMode) {
  // Set up scenario where no hosts are healthy, should trigger healthy panic
  host_set_->hosts_ = hosts_;
  host_set_->healthy_hosts_ = {}; // No healthy hosts
  host_set_->runCallbacks({}, {});

  setHostStats(hosts_[0], std::chrono::milliseconds(100), 1);
  setHostStats(hosts_[1], std::chrono::milliseconds(200), 2);

  // Force selection between hosts_[0] and hosts_[1]
  // random_value = 0: first_choice = 0, second_choice = 1
  EXPECT_CALL(random_, random()).WillOnce(Return(0));

  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[0] due to lower cost in panic mode
  EXPECT_EQ(result.host, hosts_[0]);
  // ZoneAwareLoadBalancerBase handles panic mode internally\n  // Verify that we still get a valid host selection in panic mode\n  EXPECT_NE(result.host, nullptr);
}

TEST_F(PeakEwmaLoadBalancerTest, HostNotInStatsMap) {
  // Create a host that's not in the stats map
  auto orphan_host = std::make_shared<NiceMock<Upstream::MockHost>>();
  ON_CALL(*orphan_host, address())
      .WillByDefault(Return(
          std::make_shared<Network::Address::Ipv4Instance>("10.0.0.99")));

  // Add it to the host set but don't call the update callback
  host_set_->hosts_ = {orphan_host, hosts_[1]};
  host_set_->healthy_hosts_ = {orphan_host, hosts_[1]};

  setHostStats(hosts_[1], std::chrono::milliseconds(100), 1);

  // Force selection of orphan_host vs hosts_[1]
  // random_value = 0: first_choice = 0 (orphan_host), second_choice = 1 (hosts_[1])
  EXPECT_CALL(random_, random()).WillOnce(Return(0));

  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[1] because orphan_host has max cost
  EXPECT_EQ(result.host, hosts_[1]);
}

TEST_F(PeakEwmaLoadBalancerTest, PeekAnotherHost) {
  // Test the peekAnotherHost method
  auto result = lb_->peekAnotherHost(nullptr);
  EXPECT_EQ(result, nullptr);
}

TEST_F(PeakEwmaLoadBalancerTest, LifetimeCallbacks) {
  // Test the lifetimeCallbacks method
  auto callbacks = lb_->lifetimeCallbacks();
  EXPECT_FALSE(callbacks.has_value());
}

TEST_F(PeakEwmaLoadBalancerTest, SelectExistingConnection) {
  // Test the selectExistingConnection method
  std::vector<uint8_t> hash_key;
  auto result = lb_->selectExistingConnection(nullptr, *hosts_[0], hash_key);
  EXPECT_FALSE(result.has_value());
}

TEST_F(PeakEwmaLoadBalancerTest, PenaltyBasedSystemForNewHosts) {
  // Test penalty-based system for hosts with no RTT history

  // Host 0: No RTT history, no active requests (cost = 0.0)
  hosts_[0]->stats().rq_active_.set(0);

  // Host 1: No RTT history, has active requests (cost = penalty + active_requests)
  hosts_[1]->stats().rq_active_.set(2);

  // Host 2: Has RTT history with normal cost calculation
  setHostStats(hosts_[2], std::chrono::milliseconds(50), 1);  // cost = 50 * 2 = 100

  // Force P2C to select hosts_[0] (no history, no active) vs hosts_[1] (no history, has active)
  // random_value = 0: first_choice = 0, second_choice = 1
  EXPECT_CALL(random_, random()).WillOnce(Return(0));

  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[0] because it has cost 0.0 vs penalty + 2 for hosts_[1]
  EXPECT_EQ(result.host, hosts_[0]);

  // Now test penalty vs normal cost
  // Force P2C to select hosts_[1] (penalty + 2) vs hosts_[2] (100)
  // random_value = 1: first_choice = 1, second_choice = 2
  EXPECT_CALL(random_, random()).WillOnce(Return(1));

  result = lb_->chooseHost(nullptr);
  // Should select hosts_[2] because normal cost (100) < penalty (very large number)
  EXPECT_EQ(result.host, hosts_[2]);
}

TEST_F(PeakEwmaLoadBalancerTest, HostAdditionEnablesProperCostCalculation) {
  // Test that onHostSetUpdate enables proper cost calculation for new hosts

  // Create a new host that's not in the initial setup
  auto new_host = std::make_shared<NiceMock<Upstream::MockHost>>();
  ON_CALL(*new_host, address())
      .WillByDefault(Return(
          std::make_shared<Network::Address::Ipv4Instance>("10.0.0.100")));

  // Set some active requests on the new host
  new_host->stats().rq_active_.set(5);

  // Simulate adding the host via onHostSetUpdate
  // Note: We can't directly call onHostSetUpdate with mock hosts because they don't
  // support setLbPolicyData, but we can test the end-to-end behavior by adding
  // to the host set and triggering callbacks

  // Add the host to our test host set
  std::vector<Upstream::HostSharedPtr> updated_hosts = hosts_;
  updated_hosts.push_back(new_host);
  host_set_->hosts_ = updated_hosts;
  host_set_->healthy_hosts_ = updated_hosts;

  // Trigger the callback that would call onHostSetUpdate
  host_set_->runCallbacks({new_host}, {});

  // Now test that the load balancer can properly handle this host
  // The host should be usable in cost calculations (no crash, finite result)

  // Force P2C to include the new host by using it as one of the choices
  // We'll compare it against hosts_[0] which has no RTT history and no active requests
  hosts_[0]->stats().rq_active_.set(0);  // cost = 0.0

  // Create a scenario where P2C would pick hosts_[0] vs new_host
  // new_host has 5 active requests and no RTT history, so cost = penalty + 5
  // hosts_[0] has 0 requests and no RTT history, so cost = 0.0

  // We can't easily force the exact pair selection with 4 hosts, but we can
  // verify that chooseHost doesn't crash and returns a valid host
  auto result = lb_->chooseHost(nullptr);
  EXPECT_NE(result.host, nullptr);
  EXPECT_TRUE(std::find(updated_hosts.begin(), updated_hosts.end(), result.host) != updated_hosts.end());
}

TEST_F(PeakEwmaLoadBalancerTest, HostRemovalDoesNotBreakCostCalculation) {
  // Test that host removal is handled gracefully

  // First, set up hosts with known costs
  setHostStats(hosts_[0], std::chrono::milliseconds(50), 1);  // cost = 50 * 2 = 100
  setHostStats(hosts_[1], std::chrono::milliseconds(30), 2);  // cost = 30 * 3 = 90
  setHostStats(hosts_[2], std::chrono::milliseconds(40), 3);  // cost = 40 * 4 = 160

  // Remove hosts_[1] from the host set
  std::vector<Upstream::HostSharedPtr> updated_hosts = {hosts_[0], hosts_[2]};
  host_set_->hosts_ = updated_hosts;
  host_set_->healthy_hosts_ = updated_hosts;

  // Trigger the callback that would call onHostSetUpdate
  host_set_->runCallbacks({}, {hosts_[1]});

  // Load balancer should continue to work with remaining hosts
  auto result = lb_->chooseHost(nullptr);
  EXPECT_NE(result.host, nullptr);
  // Should be one of the remaining hosts
  EXPECT_TRUE(result.host == hosts_[0] || result.host == hosts_[2]);
  // Should NOT be the removed host
  EXPECT_NE(result.host, hosts_[1]);
}

TEST_F(PeakEwmaLoadBalancerTest, FallbackLogicWhenLbPolicyDataUnavailable) {
  // Test the fallback to internal map when host->typedLbPolicyData is not available
  // This test verifies the fallback path in calculateHostCost

  // Set up host stats using the internal map (simulating the fallback case)
  // The setHostStats helper already uses the internal map, so this tests the fallback
  setHostStats(hosts_[0], std::chrono::milliseconds(80), 2);

  // Test that cost calculation works through the fallback mechanism
  // This implicitly tests that the fallback logic in calculateHostCost is working

  // Force P2C to select hosts_[0] vs hosts_[1]
  // hosts_[0] has cost = 80 * 3 = 240 (via fallback)
  // hosts_[1] has no stats, so cost depends on active requests
  hosts_[1]->stats().rq_active_.set(0);  // cost = 0.0 (no RTT, no requests)

  EXPECT_CALL(random_, random()).WillOnce(Return(0));  // Select hosts_[0] vs hosts_[1]

  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[1] because 0.0 < 240
  EXPECT_EQ(result.host, hosts_[1]);
}

TEST_F(PeakEwmaLoadBalancerTest, CalculateHostCostBranchlessLogic) {
  // Test all three branches of calculateHostCostBranchless through observable behavior

  // Branch 1: No RTT data, has active requests -> penalty + active_requests
  hosts_[0]->stats().rq_active_.set(3);
  // Don't set RTT stats, so no RTT history

  // Branch 2: Has RTT data -> rtt_ewma * (active_requests + 1)
  setHostStats(hosts_[1], std::chrono::milliseconds(50), 2);  // cost = 50 * 3 = 150

  // Branch 3: No RTT data, no active requests -> 0.0
  hosts_[2]->stats().rq_active_.set(0);
  // Don't set RTT stats, so no RTT history

  // Test Branch 3 vs Branch 2: 0.0 vs 150
  EXPECT_CALL(random_, random()).WillOnce(Return(5));  // Select hosts_[2] vs hosts_[1]
  auto result = lb_->chooseHost(nullptr);
  EXPECT_EQ(result.host, hosts_[2]);  // Should select hosts_[2] (cost 0.0)

  // Test Branch 1 vs Branch 2: penalty+3 vs 150
  // Penalty is very large, so hosts_[1] should be selected
  EXPECT_CALL(random_, random()).WillOnce(Return(0));  // Select hosts_[0] vs hosts_[1]
  result = lb_->chooseHost(nullptr);
  EXPECT_EQ(result.host, hosts_[1]);  // Should select hosts_[1] (cost 150 < penalty+3)
}

TEST_F(PeakEwmaLoadBalancerTest, PrefetchingDoesNotCrash) {
  // Test that prefetching functions don't crash
  // This provides coverage for prefetchHostDataBatch and prefetchHostData

  // Set up some stats so we have valid data to prefetch
  setHostStats(hosts_[0], std::chrono::milliseconds(50), 1);
  setHostStats(hosts_[1], std::chrono::milliseconds(30), 2);
  setHostStats(hosts_[2], std::chrono::milliseconds(40), 3);

  // The prefetching happens inside chooseHost, so just call it multiple times
  // This exercises the prefetching code paths
  for (int i = 0; i < 10; ++i) {
    auto result = lb_->chooseHost(nullptr);
    EXPECT_NE(result.host, nullptr);
    EXPECT_TRUE(std::find(hosts_.begin(), hosts_.end(), result.host) != hosts_.end());
  }

  // If we get here without crashing, prefetching is working correctly
  SUCCEED();
}


} // namespace
} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
