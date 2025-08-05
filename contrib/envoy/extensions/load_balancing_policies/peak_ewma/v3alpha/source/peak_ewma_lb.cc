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

// CostCalculator implementation
double CostCalculator::calculateCost(double rtt_ewma_ms, double active_requests, double default_rtt_ms) const {
  const bool has_rtt = (rtt_ewma_ms > 0.0);
  const bool has_requests = (active_requests > 0.0);
  
  if (!has_rtt && has_requests) {
    return kPenaltyValue + active_requests;
  } else if (has_rtt) {
    return rtt_ewma_ms * (active_requests + 1.0);
  } else {
    // No RTT and no requests: treat as having default RTT performance
    return default_rtt_ms * (active_requests + 1.0);
  }
}

// PowerOfTwoSelector implementation
Upstream::HostConstSharedPtr PowerOfTwoSelector::selectBest(
    Upstream::HostConstSharedPtr first_host, double first_cost,
    Upstream::HostConstSharedPtr second_host, double second_cost,
    uint64_t random_value) const {
  const bool costs_equal = (first_cost == second_cost);
  const bool prefer_first = costs_equal ? 
    (random_value & kTieBreakingMask) != 0 : first_cost < second_cost;
  
  return prefer_first ? first_host : second_host;
}

std::pair<size_t, size_t> PowerOfTwoSelector::generateTwoDistinctIndices(
    size_t host_count, uint64_t random_value) const {
  const size_t first_index = random_value % host_count;
  const size_t second_index = (first_index + 1 + (random_value >> 16) % (host_count - 1)) % host_count;
  return {first_index, second_index};
}

GlobalHostStats::GlobalHostStats(Upstream::HostConstSharedPtr host, Stats::Scope& scope, TimeSource& time_source)
    : time_source_(time_source),
      cost_stat_(scope.gaugeFromString(
          "peak_ewma." + host->address()->asString() + ".cost",
          Stats::Gauge::ImportMode::NeverImport)),
      ewma_rtt_stat_(scope.gaugeFromString(
          "peak_ewma." + host->address()->asString() + ".ewma_rtt_ms",
          Stats::Gauge::ImportMode::NeverImport)),
      active_requests_stat_(scope.gaugeFromString(
          "peak_ewma." + host->address()->asString() + ".active_requests",
          Stats::Gauge::ImportMode::NeverImport)),
      host_(host) {}

void GlobalHostStats::setComputedCostStat(double cost) {
  cost_stat_.set(static_cast<uint64_t>(cost));
}

void GlobalHostStats::setEwmaRttStat(double ewma_rtt_ms) {
  ewma_rtt_stat_.set(static_cast<uint64_t>(ewma_rtt_ms));
}

void GlobalHostStats::setActiveRequestsStat(double active_requests) {
  active_requests_stat_.set(static_cast<uint64_t>(active_requests));
}

void GlobalHostStats::recordRttSample(std::chrono::milliseconds rtt) {
  uint64_t timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      time_source_.monotonicTime().time_since_epoch()).count();

  // Record in thread-local single buffer
  if (load_balancer_ && host_) {
    auto& thread_data = load_balancer_->getThreadLocalData();
    thread_data.recordRttSample(host_, rtt, timestamp_ns);
  }
}

// StatsPublisher implementation
void StatsPublisher::publishHostStats(std::shared_ptr<HostEwmaSnapshot> snapshot, 
                                     std::unordered_map<Upstream::HostConstSharedPtr, std::unique_ptr<GlobalHostStats>>& all_host_stats) {
  // Publish stats for all hosts that have EWMA data
  for (const auto& [host, rtt_ewma] : snapshot->ewma_values) {
    // Create host stats if they don't exist
    if (all_host_stats.find(host) == all_host_stats.end()) {
      all_host_stats[host] = createHostStats(host);
    }
    
    // Get current values
    const double active_requests = static_cast<double>(host->stats().rq_active_.value());
    const double computed_cost = cost_calculator_.calculateCost(rtt_ewma, active_requests, default_rtt_ms_);
    
    // Publish all three stats
    auto& host_stats = all_host_stats[host];
    host_stats->setEwmaRttStat(rtt_ewma);
    host_stats->setActiveRequestsStat(active_requests);
    host_stats->setComputedCostStat(computed_cost);
  }
}

std::unique_ptr<GlobalHostStats> StatsPublisher::createHostStats(Upstream::HostConstSharedPtr host) {
  return std::make_unique<GlobalHostStats>(host, scope_, time_source_);
}

// PerThreadData implementation  
void PerThreadData::recordRttSample(Upstream::HostConstSharedPtr host, std::chrono::milliseconds rtt, uint64_t timestamp_ns) {
  // Circular buffer: overwrite oldest when at capacity
  if (write_pos_ >= kMaxSamplesPerWorker) {
    write_pos_ = 0;  // Wrap around to beginning
  }
  
  // Ensure buffer is large enough (resize on first write cycle)
  if (active_buffer_->size() <= write_pos_) {
    active_buffer_->resize(write_pos_ + 1);
  }
  
  // Write sample with host information
  (*active_buffer_)[write_pos_] = {host, RttSample{static_cast<double>(rtt.count()), timestamp_ns}};
  write_pos_++;
}

std::vector<std::pair<Upstream::HostConstSharedPtr, RttSample>>* PerThreadData::swapAndClearBuffer() {
  // Get the old active buffer before swapping
  auto* old_buffer = active_buffer_;
  
  // Simple pointer swap - switch to the other buffer
  active_buffer_ = (active_buffer_ == &buffer_a_) ? &buffer_b_ : &buffer_a_;
  
  // Clear the new active buffer and reset write position
  active_buffer_->clear();
  write_pos_ = 0;
  
  // Return pointer to old buffer for main thread processing
  return old_buffer;
}


PeakEwmaLoadBalancer::PeakEwmaLoadBalancer(
    const Upstream::PrioritySet& priority_set, const Upstream::PrioritySet* local_priority_set,
    Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
    uint32_t healthy_panic_threshold, const Upstream::ClusterInfo& cluster_info,
    TimeSource& time_source,
    const envoy::extensions::load_balancing_policies::peak_ewma::v3alpha::PeakEwma& config,
    Event::Dispatcher& main_dispatcher, ThreadLocal::TypedSlot<PerThreadData>& tls_slot)
    : ZoneAwareLoadBalancerBase(priority_set, local_priority_set, stats, runtime, random,
                                healthy_panic_threshold, absl::nullopt),
      cost_calculator_(),
      p2c_selector_(),
      stats_publisher_(cluster_info.statsScope(), time_source, cost_calculator_, 10.0),
      cluster_info_(cluster_info),
      time_source_(time_source),
      config_proto_(config),
      tau_nanos_(config_proto_.has_decay_time() ? 
          DurationUtil::durationToMilliseconds(config_proto_.decay_time()) * 1000000LL :
          kDefaultDecayTimeSeconds * 1000000000LL),
      tls_slot_(tls_slot),
      main_dispatcher_(main_dispatcher),
      aggregation_interval_(config_proto_.has_aggregation_interval() ?
          std::chrono::milliseconds(DurationUtil::durationToMilliseconds(config_proto_.aggregation_interval())) :
          std::chrono::milliseconds(100)) {
  member_update_cb_handle_ = priority_set.addMemberUpdateCb(
      [this](const Upstream::HostVector& hosts_added, const Upstream::HostVector& hosts_removed) -> absl::Status {
        onHostSetUpdate(hosts_added, hosts_removed);
        return absl::OkStatus();
      });
  
  // Initialize EWMA snapshot with default values using C++11 atomic operations
  uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      time_source_.monotonicTime().time_since_epoch()).count();
  double default_ewma_ms = 10.0;  // 10ms default RTT
  std::atomic_store(&current_ewma_snapshot_, 
                    std::make_shared<HostEwmaSnapshot>(default_ewma_ms, current_time_ns));
}

PeakEwmaLoadBalancer::~PeakEwmaLoadBalancer() {
  // EWMA snapshot cleanup is automatic via shared_ptr destructor
  
  // Explicitly clear GlobalHostStats from all hosts to ensure stats are cleaned up
  // before the ThreadLocalStoreImpl destructor runs
  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    for (const auto& host : host_set->hosts()) {
      host->setLbPolicyData(nullptr);
    }
  }
}

void PeakEwmaLoadBalancer::onHostSetUpdate(
    const Upstream::HostVector& hosts_added,
    const Upstream::HostVector& /* hosts_removed */) {
  for (const auto& host : hosts_added) {
    auto stats = std::make_unique<GlobalHostStats>(host, cluster_info_.statsScope(), time_source_);
    stats->setLoadBalancer(this); // Set reference for thread-local recording
    
    // The host takes ownership of the stats object.
    host->setLbPolicyData(std::move(stats));
  }
  
  // Start timer when first hosts are added
  if (!hosts_added.empty() && !aggregation_timer_started_) {
    startAggregationTimer();
    aggregation_timer_started_ = true;
  }
}

double PeakEwmaLoadBalancer::calculateHostCost(
    Upstream::HostConstSharedPtr host) {
  // Race-free EWMA snapshot reading using C++11 atomic shared_ptr operations
  auto ewma_snapshot = std::atomic_load(&current_ewma_snapshot_);
  if (!ewma_snapshot) {
    // Fallback if no snapshot available yet
    return cost_calculator_.calculateCost(10.0, static_cast<double>(host->stats().rq_active_.value()), 10.0);
  }
  
  // No manual reference counting needed - shared_ptr keeps snapshot alive automatically
  const double rtt_ewma = getEwmaFromSnapshot(ewma_snapshot, host);
  
  // Use the standard host active request counter (real-time)
  const double active_requests = static_cast<double>(host->stats().rq_active_.value());
  
  // Use default EWMA value from snapshot for new hosts
  const double default_rtt_ms = ewma_snapshot->default_ewma_ms;
  
  return cost_calculator_.calculateCost(rtt_ewma, active_requests, default_rtt_ms);
}


Upstream::HostConstSharedPtr PeakEwmaLoadBalancer::selectFromTwoCandidates(
    const Upstream::HostVector& hosts, uint64_t random_value) {
  const size_t host_count = hosts.size();
  const auto [first_index, second_index] = p2c_selector_.generateTwoDistinctIndices(host_count, random_value);

  const auto& first_host = hosts[first_index];
  const auto& second_host = hosts[second_index];

  const double first_cost = calculateHostCost(first_host);
  const double second_cost = calculateHostCost(second_host);

  return p2c_selector_.selectBest(first_host, first_cost, second_host, second_cost, random_value);
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

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::peekAnotherHost(ABSL_ATTRIBUTE_UNUSED Upstream::LoadBalancerContext* context) {
  return nullptr;
}

// Thread-local storage access methods
PerThreadData& PeakEwmaLoadBalancer::getThreadLocalData() {
  // The TLS slot is now eagerly initialized in the config constructor.
  auto opt_ref = tls_slot_.get();
  if (opt_ref.has_value()) {
    return opt_ref.ref();
  } else {
    // Fallback to static instance if TLS is not available (e.g. in some tests)
    static PerThreadData static_instance;
    return static_instance;
  }
}

void PeakEwmaLoadBalancer::aggregateWorkerData() {
  // This method runs on the main thread and collects RTT samples from all worker threads

  // Collect raw buffer pointers from workers - simplified single buffer approach
  auto collected_buffers = std::make_shared<std::vector<std::vector<std::pair<Upstream::HostConstSharedPtr, RttSample>>*>>();
  auto mutex = std::make_shared<absl::Mutex>();

  // Simplified worker callback - minimal disruption
  tls_slot_.runOnAllThreads([collected_buffers, mutex](OptRef<PerThreadData> obj) -> void {
    if (!obj.has_value()) {
      return;
    }
    
    // Simple single buffer swap - fast operation with minimal worker disruption
    auto* old_buffer = obj->swapAndClearBuffer();
    
    // Only collect if buffer has data
    if (!old_buffer->empty()) {
      absl::MutexLock lock(mutex.get());
      collected_buffers->emplace_back(old_buffer);
    }
  }, [this, collected_buffers]() -> void {
    // K-way merge optimization: process chronologically ordered samples directly from worker buffers
    uint64_t computation_timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        time_source_.monotonicTime().time_since_epoch()).count();
    auto new_snapshot = std::make_shared<HostEwmaSnapshot>(10.0, computation_timestamp);
    auto current_snapshot = std::atomic_load(&current_ewma_snapshot_);
    
    // Per-host EWMA state - updated incrementally during k-way merge
    absl::flat_hash_map<Upstream::HostConstSharedPtr, double> host_ewma;
    absl::flat_hash_map<Upstream::HostConstSharedPtr, uint64_t> host_last_timestamp;
    
    // Simple iterators for each worker buffer (already chronologically sorted)
    std::vector<std::vector<std::pair<Upstream::HostConstSharedPtr, RttSample>>::const_iterator> iterators;
    for (auto* buffer : *collected_buffers) {
      iterators.push_back(buffer->begin());
    }
    
    // K-way merge: process samples in chronological order across all worker buffers
    while (true) {
      // Find buffer with earliest timestamp
      size_t min_buffer = SIZE_MAX;
      uint64_t min_timestamp = UINT64_MAX;
      
      for (size_t i = 0; i < collected_buffers->size(); ++i) {
        if (iterators[i] != (*collected_buffers)[i]->end()) {
          if (iterators[i]->second.timestamp_ns < min_timestamp) {
            min_timestamp = iterators[i]->second.timestamp_ns;
            min_buffer = i;
          }
        }
      }
      
      if (min_buffer == SIZE_MAX) break; // All buffers exhausted
      
      // Process the earliest sample
      const auto& [host, sample] = *iterators[min_buffer];
      
      // Initialize or update host EWMA state
      auto [ewma_iter, inserted] = host_ewma.try_emplace(host, getEwmaFromSnapshot(current_snapshot, host));
      if (inserted && current_snapshot) {
        host_last_timestamp[host] = current_snapshot->computation_timestamp_ns;
      }
      
      // Update EWMA for this host incrementally
      int64_t time_delta = static_cast<int64_t>(sample.timestamp_ns) - static_cast<int64_t>(host_last_timestamp[host]);
      if (time_delta > 0) {
        double alpha = FastAlphaCalculator::timeGapToAlpha(time_delta, tau_nanos_);
        ewma_iter->second = ewma_iter->second + alpha * (sample.rtt_ms - ewma_iter->second);
        host_last_timestamp[host] = sample.timestamp_ns;
      }
      
      // Advance the iterator for this buffer
      ++iterators[min_buffer];
    }
    
    // Publish final EWMA snapshot to all workers using C++11 atomic operations
    new_snapshot->ewma_values = std::move(host_ewma);
    std::atomic_store(&current_ewma_snapshot_, new_snapshot);
    // Old snapshot automatically cleaned up via shared_ptr destructor
    
    // Publish stats for admin interface visibility (after snapshot is live)
    stats_publisher_.publishHostStats(new_snapshot, all_host_stats_);
    
    // Reschedule timer after aggregation is completely finished
    this->aggregation_timer_->enableTimer(aggregation_interval_);
  });
}


void PeakEwmaLoadBalancer::startAggregationTimer() {
  // Create timer for periodic aggregation
  aggregation_timer_ = main_dispatcher_.createTimer([this]() {
    onAggregationTimer();
  });
  
  // Start the timer with the configured interval
  aggregation_timer_->enableTimer(aggregation_interval_);
}

void PeakEwmaLoadBalancer::onAggregationTimer() {
  // Aggregate worker data from all threads
  aggregateWorkerData();
}

double PeakEwmaLoadBalancer::getEwmaFromSnapshot(std::shared_ptr<HostEwmaSnapshot> snapshot, Upstream::HostConstSharedPtr host) {
  auto it = snapshot->ewma_values.find(host);
  return (it != snapshot->ewma_values.end()) ? it->second : snapshot->default_ewma_ms;
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
