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

GlobalHostStats::GlobalHostStats(Upstream::HostConstSharedPtr host, int64_t tau_nanos,
                                 Stats::Scope& scope, TimeSource& time_source)
    : decay_constant_(static_cast<double>(tau_nanos)),
      default_rtt_ns_(10 * 1000000), // 10ms default RTT in nanoseconds
      time_source_(time_source),
      cost_stat_(scope.gaugeFromString(
          "peak_ewma." + host->address()->asString() + ".cost",
          Stats::Gauge::ImportMode::NeverImport)),
      host_(host) {
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

void GlobalHostStats::setComputedCostStat(double cost) {
  cost_stat_.set(static_cast<uint64_t>(cost));
}

void GlobalHostStats::recordRttSample(std::chrono::milliseconds rtt) {
  uint64_t timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      time_source_.monotonicTime().time_since_epoch()).count();

  // Phase 4: Record in thread-local circular buffers
  if (load_balancer_ && host_) {
    try {
      // Direct access to our host - no reverse lookup needed
      auto& thread_stats = load_balancer_->getOrCreateThreadLocalHostStats(host_);
      thread_stats.recordRttSample(rtt, timestamp_ns);
    } catch (const std::exception&) {
      // If thread-local recording fails, silently drop the sample
      // The timer-based aggregation system will handle EWMA updates
    }
  }
}

// PerThreadHostStats implementation
PerThreadHostStats::PerThreadHostStats(Upstream::HostConstSharedPtr host, uint32_t max_samples)
    : host_(host), active_buffer_(&buffer_a_), max_samples_(max_samples) {
  // Reserve space for efficiency
  buffer_a_.reserve(max_samples);
  buffer_b_.reserve(max_samples);
}

void PerThreadHostStats::recordRttSample(std::chrono::milliseconds rtt, uint64_t timestamp_ns) {
  // Lock-free write to active buffer
  auto* buffer = active_buffer_.load(std::memory_order_relaxed);
  
  // Circular buffer behavior: overwrite oldest when at capacity
  if (buffer->size() >= max_samples_) {
    // Simple circular: remove first element and add new one
    buffer->erase(buffer->begin());
  }
  
  buffer->emplace_back(static_cast<double>(rtt.count()), timestamp_ns);
  last_update_timestamp_ = timestamp_ns;
}

std::vector<RttSample> PerThreadHostStats::collectAndSwapBuffers() {
  // Atomic swap to inactive buffer
  auto* old_buffer = (active_buffer_.load() == &buffer_a_) ? &buffer_b_ : &buffer_a_;
  auto* new_active = active_buffer_.exchange(old_buffer, std::memory_order_acq_rel);
  
  // Copy collected samples from now-inactive buffer
  std::vector<RttSample> collected = *new_active;
  
  // Clear the collected buffer for next cycle
  new_active->clear();
  
  return collected;
}

// PerThreadData implementation
PerThreadHostStats& PerThreadData::getOrCreateHostStats(Upstream::HostConstSharedPtr host) {
  auto it = host_stats_.find(host);
  if (it == host_stats_.end()) {
    auto stats = std::make_unique<PerThreadHostStats>(host, max_samples_per_host_);
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
    Event::Dispatcher& main_dispatcher, const std::unique_ptr<ThreadLocal::TypedSlot<PerThreadData>>& tls_slot)
    : ZoneAwareLoadBalancerBase(priority_set, local_priority_set, stats, runtime, random,
                                healthy_panic_threshold, absl::nullopt),
      cluster_info_(cluster_info),
      time_source_(time_source),
      config_proto_(config),
      tau_nanos_(config_proto_.has_decay_time() ? 
          DurationUtil::durationToMilliseconds(config_proto_.decay_time()) * 1000000LL :
          kDefaultDecayTimeSeconds * 1000000000LL),
      max_samples_per_host_(config_proto_.has_max_samples_per_host() ? 
          config_proto_.max_samples_per_host().value() : kDefaultMaxSamplesPerHost),
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

  // TLS slot is now pre-initialized in config
  
  // Start the aggregation timer
  startAggregationTimer();
}

void PeakEwmaLoadBalancer::onHostSetUpdate(
    const Upstream::HostVector& hosts_added,
    const Upstream::HostVector& hosts_removed) {
  for (const auto& host : hosts_added) {
    auto stats = std::make_unique<GlobalHostStats>(host, tau_nanos_,
                                                   cluster_info_.statsScope(), time_source_);
    stats->setLoadBalancer(this); // Set reference for thread-local recording
    
    // The host takes ownership of the stats object.
    host->setLbPolicyData(std::move(stats));
  }
  for (const auto& host : hosts_removed) {
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

double PeakEwmaLoadBalancer::calculateHostCost(
    Upstream::HostConstSharedPtr host) {
  auto peak_ewma_stats_opt = host->typedLbPolicyData<GlobalHostStats>();
  
  if (!peak_ewma_stats_opt.has_value()) {
    // Host doesn't have stats - return max cost
    return std::numeric_limits<double>::max();
  }
  
  GlobalHostStats* host_stats = &peak_ewma_stats_opt.ref();

  // Use cached time to reduce syscall overhead
  const int64_t cached_time = getCachedTimeNanos();
  const double rtt_ewma = host_stats->getEwmaRttMs(cached_time);
  // Use the standard host active request counter, which is reliable.
  const double active_requests = static_cast<double>(host->stats().rq_active_.value());
  
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

  const double first_cost = calculateHostCost(first_host);
  const double second_cost = calculateHostCost(second_host);

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

Upstream::HostConstSharedPtr
PeakEwmaLoadBalancer::peekAnotherHost(ABSL_ATTRIBUTE_UNUSED Upstream::LoadBalancerContext* context) {
  return nullptr;
}

// Thread-local storage access methods
PerThreadData& PeakEwmaLoadBalancer::getThreadLocalData() {
  // The TLS slot is now eagerly initialized in the constructor.
  auto opt_ref = tls_slot_->get();
  if (opt_ref.has_value()) {
    return opt_ref.ref();
  } else {
    // Fallback to static instance if TLS is not available (e.g. in some tests)
    static PerThreadData static_instance(kDefaultMaxSamplesPerHost);
    return static_instance;
  }
}

PerThreadHostStats& PeakEwmaLoadBalancer::getOrCreateThreadLocalHostStats(Upstream::HostConstSharedPtr host) {
  return getThreadLocalData().getOrCreateHostStats(host);
}

void PeakEwmaLoadBalancer::removeThreadLocalHostStats(Upstream::HostConstSharedPtr host) {
  if (tls_slot_ && tls_slot_->currentThreadRegistered()) {
    getThreadLocalData().removeHostStats(host);
  }
}

void PeakEwmaLoadBalancer::aggregateWorkerData() {
  // This method runs on the main thread and collects RTT samples from all worker threads
  if (!tls_slot_) {
    return; // TLS not initialized
  }

  // Use a shared pointer to ensure the data survives async execution
  auto host_samples = std::make_shared<absl::flat_hash_map<Upstream::HostConstSharedPtr, std::vector<RttSample>>>();

  // Collect samples from all worker threads
  tls_slot_->runOnAllThreads([host_samples](OptRef<PerThreadData> obj) -> void {
    if (!obj.has_value()) {
      return;
    }
    
    // Iterate through all hosts in this worker thread
    for (auto& [host, stats] : obj->host_stats_) {
      // Collect and swap buffers to get RTT samples
      std::vector<RttSample> collected_samples = stats->collectAndSwapBuffers();
      
      if (!collected_samples.empty()) {
        // Add samples to the aggregated collection
        auto& aggregated = (*host_samples)[host];
        aggregated.insert(aggregated.end(), collected_samples.begin(), collected_samples.end());
      }
    }
  }, [this, host_samples]() -> void {
    // Process aggregated samples in the completion callback
    this->processAggregatedSamples(*host_samples);
  });
}

void PeakEwmaLoadBalancer::processAggregatedSamples(
    const absl::flat_hash_map<Upstream::HostConstSharedPtr, std::vector<RttSample>>& host_samples) {
  // Process aggregated samples for each host
  for (const auto& [host, samples] : host_samples) {
    if (samples.empty()) {
      continue;
    }

    // Get GlobalHostStats for this host from host policy data
    auto stats_opt = host->typedLbPolicyData<GlobalHostStats>();
    if (!stats_opt.has_value()) {
      continue; // Host was removed or doesn't have stats
    }

    GlobalHostStats* global_stats = &stats_opt.ref();
    
    // Compute new EWMA from all collected samples
    // For now, we'll use a simple approach: process samples in chronological order
    std::vector<RttSample> sorted_samples = samples;
    std::sort(sorted_samples.begin(), sorted_samples.end(), 
              [](const RttSample& a, const RttSample& b) {
                return a.timestamp_ns < b.timestamp_ns;
              });

    // Get current EWMA data as the starting point for our calculation.
    const EwmaTimestampData* initial_data = global_stats->current_ewma_data_.load();
    double current_ewma = initial_data->ewma;
    uint64_t last_timestamp = initial_data->timestamp_ns;

    // Apply each sample to update the EWMA
    for (const auto& sample : sorted_samples) {
      // Calculate time delta since last update
      int64_t time_delta_ns = static_cast<int64_t>(sample.timestamp_ns) - static_cast<int64_t>(last_timestamp);
      
      if (time_delta_ns > 0) {
        // Apply time-based decay
        double alpha = FastAlphaCalculator::timeGapToAlpha(time_delta_ns, tau_nanos_);
        current_ewma = current_ewma + alpha * (sample.rtt_ms - current_ewma);
        last_timestamp = sample.timestamp_ns;
      }
    }

    // Update the global EWMA with the new computed value
    global_stats->updateGlobalEwma(current_ewma, last_timestamp);
  }
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
  
  // Reschedule the timer for the next aggregation cycle
  aggregation_timer_->enableTimer(aggregation_interval_);
}

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
