#include "source/extensions/load_balancing_policies/peak_ewma/peak_ewma_filter_config.h"
#include "source/extensions/load_balancing_policies/peak_ewma/peak_ewma_filter.h"

#include "envoy/registry/registry.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

absl::StatusOr<Http::FilterFactoryCb> PeakEwmaFilterConfigFactory::createFilterFactoryFromProto(
    const Protobuf::Message&, const std::string&, Server::Configuration::FactoryContext&) {
  return [](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<PeakEwmaRttFilter>());
  };
}

// Register the filter factory. This makes the filter available under the name
// "envoy.filters.http.peak_ewma_rtt".
REGISTER_FACTORY(PeakEwmaFilterConfigFactory, Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
