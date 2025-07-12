#include "contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/peak_ewma_lb.h"

#include <limits>
#include <memory>

#include "envoy/upstream/upstream.h"
#include "envoy/common/optref.h"

#include "source/common/common/assert.h"
#include "source/common/common/utility.h"
#include "source/common/protobuf/utility.h"

#include "absl/base/attributes.h"
#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

GlobalHostStats::GlobalHostStats(const Upstream::Host& host, int64_t tau_nanos,
                                 Stats::Scope& scope, TimeSource& time_source)
    : decay_constant_(static_cast<double>(tau_nanos)),
      default_rtt_ns_(10 * 1000000), // 10ms default RTT in nanoseconds
      time_source_(time_source),
      cost_stat_(scope.gaugeFromString(
          "peak_ewma." + host.address()->asString() + ".cost",
          Stats::Gauge::ImportMode::NeverImport)),
      direct_ewma_(tau_nanos, 0.0) {
  // Initialize EWMA data with default values
  uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      time_source_.monotonicTime().time_since_epoch()).count();
  double initial_ewma = static_cast<double>(default_rtt_ns_) / 1000000.0; // Convert to milliseconds
  current_ewma_data_.store(new EwmaTimestampData(initial_ewma, current_time_ns));
}

GlobalHostStats::~GlobalHostStats() {
  // Clean up the atomic pointer
  delete current_ewma_data_.load();
}

double GlobalHostStats::getEwmaRttMs() const {
  int64_t current_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
      time_source_.monotonicTime().time_since_epoch()).count();
  return getEwmaRttMs(current_nanos);
}

double GlobalHostStats::getEwmaRttMs(int64_t cached_time_nanos) const {
  // Lock-free read of consistent EWMA data
  const EwmaTimestampData* data = current_ewma_data_.load();
  
  // Apply time-based decay to the EWMA value
  if (static_cast<uint64_t>(cached_time_nanos) > data->timestamp_ns) {
    int64_t time_delta_ns = cached_time_nanos - static_cast<int64_t>(data->timestamp_ns);
    double alpha = FastAlphaCalculator::timeGapToAlpha(time_delta_ns, decay_constant_);
    // Decay toward default RTT
    double default_rtt_ms = static_cast<double>(default_rtt_ns_) / 1000000.0;
    return data->ewma + alpha * (default_rtt_ms - data->ewma);
  }
  
  return data->ewma;
}

void GlobalHostStats::updateGlobalEwma(double new_ewma, uint64_t timestamp_ns) {
  // This method is called by the main thread during aggregation
  // Atomic swap of new EWMA data - lock-free for readers
  const EwmaTimestampData* new_data = new EwmaTimestampData(new_ewma, timestamp_ns);
  const EwmaTimestampData* old_data = current_ewma_data_.exchange(new_data);
  delete old_data;
}

void GlobalHostStats::recordRttSample(std::chrono::milliseconds rtt) {
  // Temporary implementation: Record RTT directly in GlobalHostStats
  // This provides immediate EWMA updating for integration tests
  // Will be replaced by per-thread aggregation in Phase 4
  std::lock_guard<std::mutex> lock(rtt_mutex_);
  
  uint64_t timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      time_source_.monotonicTime().time_since_epoch()).count();
  
  // Update the direct EWMA calculator
  direct_ewma_.insert(static_cast<double>(rtt.count()), timestamp_ns);
  
  // Update the atomic EWMA data for lock-free reads
  double current_ewma = direct_ewma_.value(timestamp_ns);
  const EwmaTimestampData* new_data = new EwmaTimestampData(current_ewma, timestamp_ns);
  const EwmaTimestampData* old_data = current_ewma_data_.exchange(new_data);
  delete old_data;
}

// PerThreadHostStats implementation
PerThreadHostStats::PerThreadHostStats(Upstream::HostConstSharedPtr host, int64_t tau_nanos)
    : host_(host), local_ewma_(tau_nanos, 0.0) {}

void PerThreadHostStats::recordRttSample(std::chrono::milliseconds rtt, uint64_t timestamp_ns) {
  local_ewma_.insert(static_cast<double>(rtt.count()), timestamp_ns);
  last_update_timestamp_ = timestamp_ns;
}

// PerThreadData implementation
PerThreadHostStats& PerThreadData::getOrCreateHostStats(Upstream::HostConstSharedPtr host, int64_t tau_nanos) {
  auto it = host_stats_.find(host);
  if (it == host_stats_.end()) {
    auto stats = std::make_unique<PerThreadHostStats>(host, tau_nanos);
    auto* stats_ptr = stats.get();
    host_stats_[host] = std::move(stats);
    return *stats_ptr;
  }
  return *it->second;
}

void PerThreadData::removeHostStats(Upstream::HostConstSharedPtr host) {
  host_stats_.erase(host);
}

PeakEwmaLoadBalancer::PeakEwmaLoadBalancer(
    const Upstream::PrioritySet& priority_set, const Upstream::PrioritySet* local_priority_set,
    Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
    uint32_t healthy_panic_threshold, const Upstream::ClusterInfo& cluster_info,
    TimeSource& time_source,
    const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config,
    ThreadLocal::SlotAllocator& tls_allocator)
    : ZoneAwareLoadBalancerBase(priority_set, local_priority_set, stats, runtime, random,
                                healthy_panic_threshold, absl::nullopt),
      cluster_info_(cluster_info),
      time_source_(time_source),
      config_proto_(config),
      tau_nanos_(config_proto_.has_decay_time() ? 
          DurationUtil::durationToMilliseconds(config_proto_.decay_time()) * 1000000LL :
          kDefaultDecayTimeSeconds * 1000000000LL),
      tls_allocator_(tls_allocator) {
  member_update_cb_handle_ = priority_set.addMemberUpdateCb(
      [this](const Upstream::HostVector& hosts_added, const Upstream::HostVector& hosts_removed) -> absl::Status {
        onHostSetUpdate(hosts_added, hosts_removed);
        return absl::OkStatus();
      });

  for (const auto& host_set : priority_set.hostSetsPerPriority()) {
    onHostSetUpdate(host_set->hosts(), {});
  }
}

void PeakEwmaLoadBalancer::onHostSetUpdate(
    const Upstream::HostVector& hosts_added,
    const Upstream::HostVector& hosts_removed) {
  for (const auto& host : hosts_added) {
    auto stats = std::make_unique<GlobalHostStats>(*host, tau_nanos_,
                                                   cluster_info_.statsScope(), time_source_);
    host->setLbPolicyData(std::move(stats));
    
    // Maintain internal map for fallback compatibility
    host_stats_map_.try_emplace(host, std::make_unique<GlobalHostStats>(*host, tau_nanos_,
                                cluster_info_.statsScope(), time_source_));
  }
  for (const auto& host : hosts_removed) {
    host_stats_map_.erase(host);
    // Clean up thread-local data for removed hosts across all threads (if TLS is initialized)
    if (tls_slot_) {
      tls_slot_->runOnAllThreads([host](OptRef<PerThreadData> obj) -> void {
        if (obj.has_value()) {
          obj->removeHostStats(host);
        }
      });
    }
  }
}

PeakEwmaLoadBalancer::HostStatIterator 
PeakEwmaLoadBalancer::findHostStats(Upstream::HostConstSharedPtr host) {
  return host_stats_map_.find(host);
}

double PeakEwmaLoadBalancer::calculateHostCost(
    Upstream::HostConstSharedPtr host, HostStatIterator& iterator) {
  auto peak_ewma_stats_opt = host->typedLbPolicyData<GlobalHostStats>();
  GlobalHostStats* host_stats = nullptr;
  
  if (peak_ewma_stats_opt.has_value()) {
    host_stats = &peak_ewma_stats_opt.ref();
  } else {
    iterator = findHostStats(host);
    if (ABSL_PREDICT_FALSE(iterator == host_stats_map_.end())) {
      return std::numeric_limits<double>::max();
    }
    host_stats = iterator->second.get();
  }

  // Use cached time to reduce syscall overhead
  const int64_t cached_time = getCachedTimeNanos();
  const double rtt_ewma = host_stats->getEwmaRttMs(cached_time);
  // Use pending requests from GlobalHostStats instead of host stats
  const double active_requests = static_cast<double>(host_stats->getPendingRequests());
  
  const double cost = calculateHostCostBranchless(rtt_ewma, active_requests);
  host_stats->setComputedCostStat(cost);
  return cost;
}

double PeakEwmaLoadBalancer::calculateHostCostBranchless(double rtt_ewma, double active_requests) const {
  const bool has_rtt = (rtt_ewma > 0.0);
  const bool has_requests = (active_requests > 0.0);
  
  if (!has_rtt && has_requests) {
    return kPenaltyValue + active_requests;
  } else if (has_rtt) {
    return rtt_ewma * (active_requests + 1.0);
  } else {
    return 0.0;
  }
}



Upstream::HostConstSharedPtr PeakEwmaLoadBalancer::selectFromTwoCandidates(
    const Upstream::HostVector& hosts, uint64_t random_value) {
  const size_t host_count = hosts.size();
  const size_t first_index = random_value % host_count;
  const size_t second_index = (first_index + 1 + (random_value >> 16) % (host_count - 1)) % host_count;

  const auto& first_host = hosts[first_index];
  const auto& second_host = hosts[second_index];

  // Use intelligent prefetching for the two specific hosts we need
  prefetchHostData(hosts, first_index, second_index);

  HostStatIterator first_iterator;
  HostStatIterator second_iterator;
  
  const double first_cost = calculateHostCost(first_host, first_iterator);
  const double second_cost = calculateHostCost(second_host, second_iterator);

  const bool costs_equal = (first_cost == second_cost);
  const bool prefer_first = costs_equal ? 
    (random_value & kTieBreakingMask) != 0 : first_cost < second_cost;
  
  return prefer_first ? first_host : second_host;
}

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::chooseHostOnce(ABSL_ATTRIBUTE_UNUSED Upstream::LoadBalancerContext* context) {
  const auto& host_sets = priority_set_.hostSetsPerPriority();
  const Upstream::HostSet* current_host_set = nullptr;
  
  for (const auto& host_set : host_sets) {
    if (host_set && !host_set->healthyHosts().empty()) {
      current_host_set = host_set.get();
      break;
    }
  }

  if (current_host_set == nullptr) {
    if (!host_sets.empty() && host_sets[0] && !host_sets[0]->hosts().empty()) {
      current_host_set = host_sets[0].get();
    } else {
      return nullptr;
    }
  }

  const auto& hosts_to_consider = current_host_set->healthyHosts().empty()
                                      ? current_host_set->hosts()
                                      : current_host_set->healthyHosts();

  if (hosts_to_consider.empty()) {
    return nullptr;
  }

  if (hosts_to_consider.size() == 1) {
    return hosts_to_consider[0];
  }

  return selectFromTwoCandidates(hosts_to_consider, random_.random());
}

int64_t PeakEwmaLoadBalancer::getCachedTimeNanos() const {
  if (++time_cache_counter_ >= kTimeCacheUpdates) {
    cached_time_nanos_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        time_source_.monotonicTime().time_since_epoch()).count();
    time_cache_counter_ = 0;
  }
  return cached_time_nanos_;
}

void PeakEwmaLoadBalancer::prefetchHostData(
    const Upstream::HostVector& hosts, size_t primary_idx, size_t secondary_idx) const {
  // Prefetch the two candidates we'll actually use
  __builtin_prefetch(hosts[primary_idx].get(), kPrefetchReadHint, kPrefetchHighLocality);
  __builtin_prefetch(hosts[secondary_idx].get(), kPrefetchReadHint, kPrefetchHighLocality);
  
  // Prefetch their stats data from the hash map
  auto it1 = host_stats_map_.find(hosts[primary_idx]);
  auto it2 = host_stats_map_.find(hosts[secondary_idx]);
  if (it1 != host_stats_map_.end()) {
    __builtin_prefetch(&it1->second, kPrefetchReadHint, kPrefetchHighLocality);
  }
  if (it2 != host_stats_map_.end()) {
    __builtin_prefetch(&it2->second, kPrefetchReadHint, kPrefetchHighLocality);
  }
}

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::peekAnotherHost(ABSL_ATTRIBUTE_UNUSED Upstream::LoadBalancerContext* context) {
  return nullptr;
}

// Thread-local storage access methods
PerThreadData& PeakEwmaLoadBalancer::getThreadLocalData() {
  // Lazy initialization of TLS slot to avoid main thread requirement
  if (!tls_slot_) {
    try {
      tls_slot_ = ThreadLocal::TypedSlot<PerThreadData>::makeUnique(tls_allocator_);
      tls_slot_->set([](Event::Dispatcher&) -> std::shared_ptr<PerThreadData> {
        return std::make_shared<PerThreadData>();
      });
    } catch (const std::exception&) {
      // TLS not available (e.g., in tests or certain contexts), return a static instance
      static PerThreadData static_instance;
      return static_instance;
    }
  }
  
  auto opt_ref = tls_slot_->get();
  if (opt_ref.has_value()) {
    return opt_ref.ref();
  } else {
    // Fallback to static instance if TLS is not available
    static PerThreadData static_instance;
    return static_instance;
  }
}

PerThreadHostStats& PeakEwmaLoadBalancer::getOrCreateThreadLocalHostStats(Upstream::HostConstSharedPtr host) {
  return getThreadLocalData().getOrCreateHostStats(host, tau_nanos_);
}

void PeakEwmaLoadBalancer::removeThreadLocalHostStats(Upstream::HostConstSharedPtr host) {
  if (tls_slot_ && tls_slot_->currentThreadRegistered()) {
    getThreadLocalData().removeHostStats(host);
  }
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
