#pragma once

#include "envoy/upstream/host_description.h" // For Upstream::HostLbPolicyData
#include "envoy/upstream/load_balancer.h"
#include "envoy/upstream/upstream.h"
#include "envoy/common/time.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/stats/scope.h"
#include "envoy/runtime/runtime.h"

#include "source/common/upstream/load_balancer_impl.h" // For LoadBalancerBase an utilities
#include "source/extensions/load_balancing_policies/peak_ewma/ewma.h" // Your EWMA implementation


#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/peak_ewma.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

/**
 * All Peak EWMA load balancer stats. @see stats_macros.h
 */
#define ALL_PEAK_EWMA_LOAD_BALANCER_STATS(GAUGE)                                                   \
  GAUGE(peak_ewma_ewma_rtt_ms, NeverImport)                                                        \
  GAUGE(peak_ewma_computed_cost, NeverImport)

/**
 * Struct definition for all Peak EWMA load balancer stats. @see stats_macros.h
 */
struct PeakEwmaLbStats {
  ALL_PEAK_EWMA_LOAD_BALANCER_STATS(GENERATE_GAUGE_STRUCT)
};

struct PeakEwmaHostStats : public Upstream::HostLbPolicyData {
  PeakEwmaHostStats(double smoothing_factor, double default_rtt_ms, TimeSource& time_source,
                    Stats::Scope& host_scope, const Upstream::HostDescription& host_for_stat_name)
      : ewma_rtt_calculator_(smoothing_factor, default_rtt_ms), // Use your EwmaCalculator
        default_rtt_ms_(default_rtt_ms),
        time_source_(time_source),
        last_rtt_update_time_(time_source.monotonicTime()),
        stats_scope_(host_scope.createScope(
            Stats::Utility::sanitizeStatsName(fmt::format("peak_ewma.{}", host_for_stat_name.address()->asString())))),
        stats_({ALL_PEAK_EWMA_LOAD_BALANCER_STATS(POOL_GAUGE_PREFIX(*stats_scope_, ""))}) {
    stats_.peak_ewma_ewma_rtt_ms_.set(default_rtt_ms_);
  }

  // Internal method to update EWMA (as you had before)
  void recordRttSample(std::chrono::milliseconds rtt_sample) {
    const double rtt_sample_ms = static_cast<double>(rtt_sample.count());
    last_rtt_update_time_ = time_source_.monotonicTime();
    ewma_rtt_calculator_.insert(rtt_sample_ms);
    stats_.peak_ewma_ewma_rtt_ms_.set(ewma_rtt_calculator_.value());
     ENVOY_LOG(trace, "PeakEwmaHostStats for {} updated EWMA to: {}ms with sample {}ms",
               stats_scope_->symbolTable().toString(stats_scope_->prefix()), /* Using scope prefix as identifier */
               ewma_rtt_calculator_.value(), rtt_sample_ms);
  }

  // == OVERRIDE THE NEW VIRTUAL METHOD from Upstream::HostLbPolicyData ==
  void onHostRttReported(std::chrono::milliseconds rtt) override {
    recordRttSample(rtt);
  }
  // == END OF OVERRIDE ==

  double getEwmaRttMs() const { return ewma_rtt_calculator_.value(); }

  void setComputedCostStat(double cost) {
    stats_.peak_ewma_computed_cost_.set(cost);
  }

  EwmaCalculator ewma_rtt_calculator_;
  const double default_rtt_ms_;
  TimeSource& time_source_;
  MonotonicTime last_rtt_update_time_;

  Stats::ScopePtr stats_scope_;
  PeakEwmaLbStats stats_;
};

// Per-thread Peak EWMA load balancer.
class PeakEwmaLoadBalancer : public Upstream::LoadBalancerBase {
public:
  PeakEwmaLoadBalancer(
      const Upstream::PrioritySet& priority_set, const Upstream::HostSet* local_host_set,
      Upstream::ClusterLbStats& stats, Runtime::Loader& runtime,
      Random::RandomGenerator& random, TimeSource& time_source,
      const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config,
      const envoy::config::cluster::v3::Cluster::CommonLbConfig& common_config);

  // Upstream::LoadBalancer
  Upstream::HostConstSharedPtr chooseHost(Upstream::LoadBalancerContext* context) override;

private:
  void initializeHostStats(const Upstream::HostSharedPtr& host);
  void onHostAdded(const Upstream::HostSharedPtr& host);

  TimeSource& time_source_;
  const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma config_proto_;
  const double default_rtt_ms_;
  const double smoothing_factor_;

  // Callback for host set member updates.
  // This is stored to keep the lambda alive.
  Common::CallbackHandlePtr member_update_cb_handle_;
  // If supporting local host sets for zone-aware routing, a separate callback handle might be needed.
  Common::CallbackHandlePtr local_member_update_cb_handle_;
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
