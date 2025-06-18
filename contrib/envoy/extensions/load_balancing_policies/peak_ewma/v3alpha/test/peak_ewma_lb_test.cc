#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"

#include "source/common/upstream/upstream_impl.h"

#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

// Bring testing namespace into scope for cleaner syntax
using ::testing::Return;
using ::testing::ReturnRef;

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {
namespace {

// A test-only "peer" class to access private members of the load balancer for testing.
class PeakEwmaTestPeer {
public:
  static absl::flat_hash_map<Upstream::HostConstSharedPtr, PeakEwmaHostStats>&
  hostStatsMap(PeakEwmaLoadBalancer& lb) {
    return lb.host_stats_map_;
  }
};

class PeakEwmaLoadBalancerTest : public ::testing::Test {
public:
  PeakEwmaLoadBalancerTest() {
    // Set up 3 mock hosts for the test.
    for (int i = 0; i < 3; ++i) {
      hosts_.emplace_back(std::make_shared<NiceMock<Upstream::MockHost>>());
      ON_CALL(*hosts_[i], stats()).WillByDefault(ReturnRef(host_stats_[i]));
    }
    
    // Create shared pointers for the host vectors, as required by HostSetImpl::updateHosts.
    auto hosts_ptr = std::make_shared<Upstream::HostVector>(hosts_.begin(), hosts_.end());
    auto healthy_hosts_ptr = std::make_shared<Upstream::HealthyHostVector>(hosts_.begin(), hosts_.end());

    // Use a real HostSetImpl and populate it correctly via its public updateHosts method.
    host_set_ = std::make_unique<Upstream::HostSetImpl>(0, absl::nullopt, absl::nullopt);
    host_set_->updateHosts(
        Upstream::HostSetImpl::updateHostsParams(
            hosts_ptr, Upstream::HostsPerLocalityImpl::empty(), healthy_hosts_ptr,
            Upstream::HostsPerLocalityImpl::empty(), nullptr,
            Upstream::HostsPerLocalityImpl::empty(), nullptr, Upstream::HostsPerLocalityImpl::empty()),
        nullptr, {}, {}, 0, absl::nullopt, absl::nullopt);

    // Set up the mock priority set to return our host set.
    host_sets_.emplace_back(std::move(host_set_));
    ON_CALL(priority_set_, hostSetsPerPriority()).WillByDefault(ReturnRef(host_sets_));
  }

  void initialize(const std::string& yaml_config) {
    TestUtility::loadFromYaml(yaml_config, config_);
    lb_ = std::make_unique<PeakEwmaLoadBalancer>(
        Upstream::LoadBalancerParams{priority_set_, &priority_set_}, *cluster_info_,
        cluster_info_->lbStats(), runtime_, random_, time_source_, config_);
  }

  // Helper to manually set the EWMA RTT for a host for predictable testing.
  void setHostStats(size_t host_index, uint64_t active_requests, double ewma_rtt) {
    host_stats_[host_index].rq_active_.set(active_requests);

    // Use the peer class to access the private host_stats_map_ for testing.
    auto& map = PeakEwmaTestPeer::hostStatsMap(*lb_);
    auto it = map.find(hosts_[host_index]);

    // Ensure the host was found in the map before trying to modify it.
    ASSERT_NE(it, map.end());

    // Directly reset the EwmaCalculator to a known value.
    it->second.rtt_ewma_.reset(ewma_rtt);
  }

  std::shared_ptr<NiceMock<Upstream::MockClusterInfo>> cluster_info_{
      std::make_shared<NiceMock<Upstream::MockClusterInfo>>()};
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  std::vector<Upstream::HostSetPtr> host_sets_;
  std::unique_ptr<Upstream::HostSetImpl> host_set_;
  std::vector<std::shared_ptr<Upstream::MockHost>> hosts_;
  std::array<Upstream::HostStats, 3> host_stats_;
  NiceMock<Runtime::MockLoader> runtime_;
  // Corrected mock types.
  NiceMock<Random::MockRandomGenerator> random_;
  NiceMock<MockTimeSystem> time_source_;
  envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_;
  std::unique_ptr<PeakEwmaLoadBalancer> lb_;
};

TEST_F(PeakEwmaLoadBalancerTest, SelectsHostWithLowestCost) {
  const std::string yaml = R"EOF(
    default_rtt: 100ms
    rtt_smoothing_factor: 0.5
  )EOF";
  initialize(yaml);

  // Cost = RTT * (active_requests + 1)
  setHostStats(0, 1, 20.0); // Cost = 40
  setHostStats(1, 2, 10.0); // Cost = 30 (min)
  setHostStats(2, 10, 5.0); // Cost = 55

  // Correctly access the .host member of the response struct.
  EXPECT_EQ(lb_->chooseHost(nullptr).host, hosts_[1]);
}

TEST_F(PeakEwmaLoadBalancerTest, TieBreaking) {
  const std::string yaml = R"EOF(
    default_rtt: 100ms
    rtt_smoothing_factor: 0.5
  )EOF";
  initialize(yaml);

  setHostStats(0, 4, 20.0); // Cost = 100
  setHostStats(1, 9, 10.0); // Cost = 100
  setHostStats(2, 5, 50.0); // Cost = 300

  // Use the 'testing::' namespace for GMock actions.
  EXPECT_CALL(random_, random()).WillOnce(testing::Return(0));
  EXPECT_EQ(lb_->chooseHost(nullptr).host, hosts_[0]);

  EXPECT_CALL(random_, random()).WillOnce(testing::Return(1));
  EXPECT_EQ(lb_->chooseHost(nullptr).host, hosts_[1]);
}

} // namespace
} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
