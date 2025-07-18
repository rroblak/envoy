#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"
#include "envoy/config/cluster/v3/cluster.pb.h"

#include "source/common/common/logger.h"
#include "source/common/network/address_impl.h"

#include "test/common/stats/stat_test_utility.h"
#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
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

// Simple ThreadLocal mock for testing
class MockThreadLocalInstance : public ThreadLocal::SlotAllocator {
public:
  ThreadLocal::SlotPtr allocateSlot() override {
    return std::make_unique<MockSlot>();
  }

private:
  class MockSlot : public ThreadLocal::Slot {
  public:
    bool currentThreadRegistered() override { return true; }
    ThreadLocal::ThreadLocalObjectSharedPtr get() override { return nullptr; }
    void set(InitializeCb) override {}
    void runOnAllThreads(const UpdateCb&) override {}
    void runOnAllThreads(const UpdateCb&, const std::function<void()>&) override {}
    bool isShutdown() const override { return false; }
  };
};

namespace {

class PeakEwmaTestPeer {
public:
  PeakEwmaTestPeer() = default;
  // Helper to get GlobalHostStats from host policy data
  GlobalHostStats* getHostStats(Upstream::HostConstSharedPtr host) {
    auto stats_opt = host->typedLbPolicyData<GlobalHostStats>();
    return stats_opt.has_value() ? &stats_opt.ref() : nullptr;
  }
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

    // Create TLS slot for testing (similar to config)
    const uint32_t max_samples = config_.has_max_samples_per_host() ? 
        config_.max_samples_per_host().value() : 25000;
    tls_slot_ = ThreadLocal::TypedSlot<PerThreadData>::makeUnique(tls_);
    tls_slot_->set([max_samples](Event::Dispatcher&) -> std::shared_ptr<PerThreadData> {
      return std::make_shared<PerThreadData>(max_samples);
    });

    lb_ = std::make_unique<PeakEwmaLoadBalancer>(
        params.priority_set, params.local_priority_set, stats_, runtime_, random_, 50,
        *cluster_info_, time_source_, config_, dispatcher_, tls_slot_);

    // Trigger the member update callback by calling runCallbacks
    // Pass hosts_ as added hosts to ensure GlobalHostStats are created
    // Must call priority_set_.runUpdateCallbacks() to trigger the PrioritySet callbacks
    // that the load balancer actually registered with
    priority_set_.runUpdateCallbacks(0, hosts_, {});
    
    // Verify GlobalHostStats were created - if not, create them manually
    // This works around the mock callback issue
    for (const auto& host : hosts_) {
      if (!host->typedLbPolicyData<GlobalHostStats>().has_value()) {
        auto stats = std::make_unique<GlobalHostStats>(
            host, 
            lb_->tau_nanos_,
            lb_->cluster_info_.statsScope(), 
            lb_->time_source_);
        stats->setLoadBalancer(lb_.get());
        host->setLbPolicyData(std::move(stats));
      }
    }
  }

  void setHostStats(Upstream::HostConstSharedPtr host, std::chrono::milliseconds rtt,
                    uint32_t active_requests) {
    host->stats().rq_active_.set(active_requests);

    // Get GlobalHostStats from host policy data
    PeakEwmaTestPeer peer;
    auto* host_stats = peer.getHostStats(host);
    ASSERT_NE(host_stats, nullptr) << "Host " << host->address()->asString() << " has no GlobalHostStats";
    
    // Update the GlobalHostStats with new EWMA data
    uint64_t current_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
        time_source_.monotonicTime().time_since_epoch()).count();
    host_stats->updateGlobalEwma(static_cast<double>(rtt.count()), current_time);
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
  NiceMock<Event::MockDispatcher> dispatcher_;
  MockThreadLocalInstance tls_;
  std::unique_ptr<ThreadLocal::TypedSlot<PerThreadData>> tls_slot_;
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
  EXPECT_EQ(result.host->address(), hosts_[1]->address());
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
  EXPECT_EQ(result.host->address(), hosts_[1]->address());

  // Test the other tie-breaking case
  // random_value with high bit set: selects host1 (hosts_[0])
  EXPECT_CALL(random_, random()).WillOnce(Return(0x8000000000000000ULL));

  result = lb_->chooseHost(nullptr);
  // Should select hosts_[0] because high bit is 1
  EXPECT_EQ(result.host->address(), hosts_[0]->address());
}

TEST_F(PeakEwmaLoadBalancerTest, P2CSingleHost) {
  // Remove all but one host
  host_set_->hosts_ = {hosts_[0]};
  host_set_->healthy_hosts_ = {hosts_[0]};
  priority_set_.runUpdateCallbacks(0, {hosts_[0]}, {hosts_[1], hosts_[2]});

  setHostStats(hosts_[0], std::chrono::milliseconds(100), 1);

  auto result = lb_->chooseHost(nullptr);
  // Should select the only host without calling random
  EXPECT_EQ(result.host->address(), hosts_[0]->address());
}

TEST_F(PeakEwmaLoadBalancerTest, NoHealthyHosts) {
  // Set up hosts but no healthy hosts
  host_set_->hosts_ = hosts_;
  host_set_->healthy_hosts_ = {}; // No healthy hosts
  priority_set_.runUpdateCallbacks(0, {}, {});

  setHostStats(hosts_[0], std::chrono::milliseconds(100), 1);
  setHostStats(hosts_[1], std::chrono::milliseconds(200), 2);
  setHostStats(hosts_[2], std::chrono::milliseconds(300), 3);

  // Should fall back to all hosts and use P2C among them
  // random_value = 0: first_choice = 0, second_choice = 1
  EXPECT_CALL(random_, random()).WillOnce(Return(0));

  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[0] because it has lower cost (200 < 600)
  EXPECT_EQ(result.host->address(), hosts_[0]->address());
}

TEST_F(PeakEwmaLoadBalancerTest, NoHostsAvailable) {
  // Remove all hosts
  host_set_->hosts_ = {};
  host_set_->healthy_hosts_ = {};
  priority_set_.runUpdateCallbacks(0, {}, {hosts_[0], hosts_[1], hosts_[2]});

  auto result = lb_->chooseHost(nullptr);
  // Should return nullptr when no hosts available
  EXPECT_EQ(result.host, nullptr);
}

TEST_F(PeakEwmaLoadBalancerTest, HostStatsUpdate) {
  setHostStats(hosts_[0], std::chrono::milliseconds(100), 5);

  // Verify host stats are properly stored and can be retrieved from host policy data
  PeakEwmaTestPeer peer;
  auto* host_stats = peer.getHostStats(hosts_[0]);
  ASSERT_NE(host_stats, nullptr);

  // Check that EWMA value was set correctly (account for time decay)
  EXPECT_NEAR(host_stats->getEwmaRttMs(), 100.0, 10.0);  // Allow 10ms tolerance for time decay

  // Update stats and verify EWMA updates
  uint64_t current_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
      time_source_.monotonicTime().time_since_epoch()).count();
  host_stats->updateGlobalEwma(200.0, current_time);
  // With time-based EWMA: peak sensitivity increases learning rate for spikes
  // Expected: weighted blend between 100 and 200, closer to 200 due to peak sensitivity
  EXPECT_GT(host_stats->getEwmaRttMs(), 100.0);  // Should be greater than original
  EXPECT_LT(host_stats->getEwmaRttMs(), 200.0);  // But not a full reset to 200
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
  EXPECT_EQ(result.host->address(), hosts_[0]->address());
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
  EXPECT_TRUE(result1.host->address() == hosts_[0]->address() || result1.host->address() == hosts_[1]->address());

  // Test second combination: different random value should still select a valid host
  // random_value = 0x8000000000010001ULL gives: first_choice=1, second_choice=0, high_bit=1
  EXPECT_CALL(random_, random()).WillOnce(Return(0x8000000000010001ULL));

  auto result2 = lb_->chooseHost(nullptr);
  // Should select either hosts_[0] or hosts_[1] - both are valid with similar costs
  EXPECT_TRUE(result2.host->address() == hosts_[0]->address() || result2.host->address() == hosts_[1]->address());
}

TEST_F(PeakEwmaLoadBalancerTest, HealthyPanicMode) {
  // Set up scenario where no hosts are healthy, should trigger healthy panic
  host_set_->hosts_ = hosts_;
  host_set_->healthy_hosts_ = {}; // No healthy hosts
  priority_set_.runUpdateCallbacks(0, {}, {});

  setHostStats(hosts_[0], std::chrono::milliseconds(100), 1);
  setHostStats(hosts_[1], std::chrono::milliseconds(200), 2);

  // Force selection between hosts_[0] and hosts_[1]
  // random_value = 0: first_choice = 0, second_choice = 1
  EXPECT_CALL(random_, random()).WillOnce(Return(0));

  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[0] due to lower cost in panic mode
  EXPECT_EQ(result.host->address(), hosts_[0]->address());
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
  EXPECT_EQ(result.host->address(), hosts_[1]->address());
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
  // Test cost calculation for hosts with no RTT history (i.e., new hosts).
  // These hosts are assigned a default EWMA RTT of 10ms.

  // Host 0: No RTT history, no active requests. Cost = 10 * (0 + 1) = 10.
  hosts_[0]->stats().rq_active_.set(0);

  // Host 1: No RTT history, 2 active requests. Cost = 10 * (2 + 1) = 30.
  hosts_[1]->stats().rq_active_.set(2);

  // Host 2: Has RTT history. Cost = 50 * (1 + 1) = 100.
  setHostStats(hosts_[2], std::chrono::milliseconds(50), 1);

  // Force P2C to select between hosts_[0] (cost 10) and hosts_[1] (cost 30).
  EXPECT_CALL(random_, random()).WillOnce(Return(0));
  auto result = lb_->chooseHost(nullptr);
  // hosts_[0] should be chosen as it has the lower cost.
  EXPECT_EQ(result.host->address(), hosts_[0]->address());

  // Force P2C to select between hosts_[1] (cost 30) and hosts_[2] (cost 100).
  EXPECT_CALL(random_, random()).WillOnce(Return(1));
  result = lb_->chooseHost(nullptr);
  // hosts_[1] should be chosen as it has the lower cost.
  EXPECT_EQ(result.host->address(), hosts_[1]->address());
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

  // Add the host to our test host set
  std::vector<Upstream::HostSharedPtr> updated_hosts = hosts_;
  updated_hosts.push_back(new_host);
  host_set_->hosts_ = updated_hosts;
  host_set_->healthy_hosts_ = updated_hosts;

  // Trigger the callback that would call onHostSetUpdate
  priority_set_.runUpdateCallbacks(0, {new_host}, {});

  // Now test that the load balancer can properly handle this host
  // The host should be usable in cost calculations (no crash, finite result)

  // Force P2C to include the new host by using it as one of the choices
  // We'll compare it against hosts_[0] which has no RTT history and no active requests
  hosts_[0]->stats().rq_active_.set(0);  // cost = 0.0

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

  // Remove hosts_[1] from the host set by triggering the callback.
  // This is the correct way to signal a host set change to the load balancer.
  priority_set_.runUpdateCallbacks(0, {}, {hosts_[1]});

  // Update the underlying host set to reflect the removal for subsequent P2C choices.
  host_set_->hosts_ = {hosts_[0], hosts_[2]};
  host_set_->healthy_hosts_ = {hosts_[0], hosts_[2]};

  // Load balancer should continue to work with remaining hosts
  auto result = lb_->chooseHost(nullptr);
  EXPECT_NE(result.host, nullptr);
  // Should be one of the remaining hosts
  EXPECT_TRUE(result.host->address() == hosts_[0]->address() || result.host->address() == hosts_[2]->address());
  // Should NOT be the removed host
  EXPECT_NE(result.host->address(), hosts_[1]->address());
}

TEST_F(PeakEwmaLoadBalancerTest, CalculateHostCostBranchlessLogic) {
  // Test the cost calculation logic with different host states.

  // Host 0: No RTT history, 3 active requests. Cost = 10 * (3 + 1) = 40.
  hosts_[0]->stats().rq_active_.set(3);

  // Host 1: Has RTT history. Cost = 50 * (2 + 1) = 150.
  setHostStats(hosts_[1], std::chrono::milliseconds(50), 2);

  // Host 2: No RTT history, no active requests. Cost = 10 * (0 + 1) = 10.
  hosts_[2]->stats().rq_active_.set(0);

  // Force P2C to select between hosts_[2] (cost 10) and hosts_[0] (cost 40).
  EXPECT_CALL(random_, random()).WillOnce(Return(5));  // Selects hosts_[2] and hosts_[0]
  auto result = lb_->chooseHost(nullptr);
  // Should select hosts_[2] as it has the lowest cost.
  EXPECT_EQ(result.host->address(), hosts_[2]->address());

  // Force P2C to select between hosts_[0] (cost 40) and hosts_[1] (cost 150).
  EXPECT_CALL(random_, random()).WillOnce(Return(0));  // Selects hosts_[0] and hosts_[1]
  result = lb_->chooseHost(nullptr);
  // Should select hosts_[0] as it has the lower cost.
  EXPECT_EQ(result.host->address(), hosts_[0]->address());
}

} // namespace
} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy