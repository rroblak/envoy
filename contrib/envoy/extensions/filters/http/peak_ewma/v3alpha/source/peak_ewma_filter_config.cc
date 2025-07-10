#include "contrib/envoy/extensions/filters/http/peak_ewma/v3alpha/source/peak_ewma_filter_config.h"
#include "contrib/envoy/extensions/filters/http/peak_ewma/v3alpha/source/peak_ewma_filter.h"

#include "envoy/registry/registry.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace PeakEwma {

Http::FilterFactoryCb PeakEwmaFilterConfigFactory::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::peak_ewma::v3alpha::PeakEwmaConfig&,
    const std::string&, Server::Configuration::FactoryContext&) {
  return [](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<PeakEwmaRttFilter>());
  };
}

REGISTER_FACTORY(PeakEwmaFilterConfigFactory, Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace PeakEwma
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
