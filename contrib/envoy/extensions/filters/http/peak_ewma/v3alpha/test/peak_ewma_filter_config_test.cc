#include "contrib/envoy/extensions/filters/http/peak_ewma/v3alpha/source/peak_ewma_filter_config.h"

#include "test/mocks/server/factory_context.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace PeakEwma {
namespace {

TEST(PeakEwmaFilterConfigTest, FactoryRegistration) {
  // Verify that the factory is properly registered
  auto factory = Registry::FactoryRegistry<Server::Configuration::NamedHttpFilterConfigFactory>::getFactory(
      "envoy.filters.http.peak_ewma");
  EXPECT_NE(factory, nullptr);
  EXPECT_EQ(factory->name(), "envoy.filters.http.peak_ewma");
}

TEST(PeakEwmaFilterConfigTest, CreateFilterFactory) {
  PeakEwmaFilterConfigFactory factory;
  NiceMock<Server::Configuration::MockFactoryContext> context;
  
  // Create an empty config proto
  envoy::extensions::filters::http::peak_ewma::v3alpha::PeakEwmaConfig proto_config;
  
  // Create the filter factory
  auto filter_factory = factory.createFilterFactoryFromProtoTyped(
      proto_config, "test_prefix", context);
  
  EXPECT_NE(filter_factory, nullptr);
  
  // Verify that the factory can create a filter
  NiceMock<Http::MockFilterChainFactoryCallbacks> filter_callbacks;
  EXPECT_CALL(filter_callbacks, addStreamFilter(_));
  
  filter_factory(filter_callbacks);
}

TEST(PeakEwmaFilterConfigTest, CreateEmptyConfigProto) {
  PeakEwmaFilterConfigFactory factory;
  auto proto = factory.createEmptyConfigProto();
  EXPECT_NE(proto, nullptr);
  
  // Verify it's the right type
  const auto* typed_proto = dynamic_cast<
      const envoy::extensions::filters::http::peak_ewma::v3alpha::PeakEwmaConfig*>(proto.get());
  EXPECT_NE(typed_proto, nullptr);
}

TEST(PeakEwmaFilterConfigTest, FactoryName) {
  PeakEwmaFilterConfigFactory factory;
  EXPECT_EQ(factory.name(), "envoy.filters.http.peak_ewma");
}

TEST(PeakEwmaFilterConfigTest, ConfigTypeUrl) {
  PeakEwmaFilterConfigFactory factory;
  EXPECT_EQ(factory.configTypes().size(), 1);
  EXPECT_EQ(factory.configTypes()[0], 
           "type.googleapis.com/envoy.extensions.filters.http.peak_ewma.v3alpha.PeakEwmaConfig");
}

TEST(PeakEwmaFilterConfigTest, CreateRouteSpecificFilterConfig) {
  PeakEwmaFilterConfigFactory factory;
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  
  // Test that route-specific config is not supported (should return nullptr)
  envoy::extensions::filters::http::peak_ewma::v3alpha::PeakEwmaConfig proto_config;
  auto route_config = factory.createRouteSpecificFilterConfig(
      proto_config, context, ProtobufMessage::getNullValidationVisitor());
  
  // Peak EWMA filter doesn't use route-specific config
  EXPECT_EQ(route_config, nullptr);
}

} // namespace
} // namespace PeakEwma
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy