#pragma once

#include "envoy/server/filter_config.h"
#include "source/extensions/filters/http/common/factory_base.h"

#include "google/protobuf/empty.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace PeakEwma {

// This configuration factory is needed to register the PeakEwmaRttFilter with Envoy.
// The filter itself takes no configuration, so it uses google.protobuf.Empty.
class PeakEwmaFilterConfigFactory
    : public Server::Configuration::NamedHttpFilterConfigFactory {
public:
  absl::StatusOr<Http::FilterFactoryCb>
  createFilterFactoryFromProto(const Protobuf::Message&, const std::string&,
                               Server::Configuration::FactoryContext&) override;

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<google::protobuf::Empty>();
  }

  std::string name() const override { return "envoy.filters.http.peak_ewma_rtt"; }
};

} // namespace PeakEwma
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
