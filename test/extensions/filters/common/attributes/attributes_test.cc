#include "source/common/network/utility.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/router/string_accessor_impl.h"
#include "source/common/stream_info/filter_state_impl.h"
#include "source/extensions/filters/common/attributes/attributes.h"

#include "test/common/stream_info/test_util.h"
#include "test/mocks/ssl/mocks.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/mocks/upstream/host.h"

#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Extensions {
namespace Filters {
namespace Common {
namespace Attributes {
namespace {

std::unique_ptr<Envoy::TestStreamInfo> makeStreamInfo() {
  auto stream_info = std::make_unique<Envoy::TestStreamInfo>();
  return stream_info;
}

TEST(Attributes, DefaultStream) {
  auto stream_info = makeStreamInfo();
  auto attrs = Attributes(*stream_info);
  std::vector<AttributeId> v;
  auto value = attrs.buildAttributesValue(v);
  std::cerr << "done building\n";
  EXPECT_TRUE(value.has_map_value());
  auto map_value = value.map_value();
  EXPECT_EQ(0, map_value.entries_size());
}
TEST(Attributes, StreamValues) {
  {
    // todo: populate this
    auto stream_info = makeStreamInfo();
    stream_info->downstream_address_provider_->setConnectionID(123);

    std::vector<AttributeId> v;
    v.push_back(AttributeId::from_path("connection.id").value());

    auto attrs = Attributes(*stream_info);
    std::cerr << "StreamValues1\n";
    auto value = attrs.buildAttributesValue(v);
    std::cerr << "StreamValues2\n";
    EXPECT_TRUE(value.has_map_value());
    auto map_value = value.map_value();
    EXPECT_EQ(1, map_value.entries_size());

    auto entry0 = map_value.entries(0);
    EXPECT_EQ(entry0.key().string_value(), "connection");
    auto entry0_value = entry0.value();
    EXPECT_TRUE(entry0_value.has_map_value());
    auto entry0_value_map = entry0_value.map_value();
    EXPECT_EQ(1, entry0_value_map.entries_size());

    auto entry0_entry0 = entry0_value_map.entries(0);
    EXPECT_TRUE(entry0_entry0.has_key());
    EXPECT_EQ(entry0_entry0.key().string_value(), "id");
    EXPECT_TRUE(entry0_entry0.value().has_uint64_value());
    EXPECT_EQ(entry0_entry0.value().uint64_value(), 123);
  }
  {
    // auto stream_info = makeStreamInfo();

    // // todo: add attrs that are getted from streaminfo
    // std::vector<AttributeId> v;
    // v.push_back(AttributeId::from_path("connection.id").value());
    // v.push_back(AttributeId::from_path("connection.mtls").value());
    // v.push_back(AttributeId::from_path("connection.requested_server_name").value());

    // v.push_back(AttributeId::from_path("connection.tls_version").value());
    // v.push_back(AttributeId::from_path("connection.subject_local_certificate").value());
    // v.push_back(AttributeId::from_path("connection.subject_peer_certificate").value());
    // v.push_back(AttributeId::from_path("connection.dns_san_local_certificate").value());
    // v.push_back(AttributeId::from_path("connection.dns_san_peer_certificate").value());
    // v.push_back(AttributeId::from_path("connection.uri_san_local_certificate").value());
    // v.push_back(AttributeId::from_path("connection.uri_san_peer_certificate").value());
    // v.push_back(AttributeId::from_path("connection.termination_details").value());

    // v.push_back(AttributeId::from_path("request.protocol").value());
    // v.push_back(AttributeId::from_path("request.duration").value());
    // v.push_back(AttributeId::from_path("request.size").value());
    // v.push_back(AttributeId::from_path("request.total_size").value());

    // v.push_back(AttributeId::from_path("response.code").value());
    // v.push_back(AttributeId::from_path("response.code_details").value());
    // v.push_back(AttributeId::from_path("response.flags").value());
    // v.push_back(AttributeId::from_path("response.size").value());
    // v.push_back(AttributeId::from_path("response.total_size").value());

    // v.push_back(AttributeId::from_path("source.address").value());
    // v.push_back(AttributeId::from_path("source.port").value());

    // v.push_back(AttributeId::from_path("destination.address").value());
    // v.push_back(AttributeId::from_path("destination.port").value());

    // v.push_back(AttributeId::from_path("upstream.address").value());
    // v.push_back(AttributeId::from_path("upstream.port").value());
    // v.push_back(AttributeId::from_path("upstream.tls_version").value());
    // v.push_back(AttributeId::from_path("upstream.subject_local_certificate").value());
    // v.push_back(AttributeId::from_path("upstream.subject_peer_certificate").value());
    // v.push_back(AttributeId::from_path("upstream.dns_san_local_certificate").value());
    // v.push_back(AttributeId::from_path("upstream.dns_san_peer_certificate").value());
    // v.push_back(AttributeId::from_path("upstream.uri_san_local_certificate").value());
    // v.push_back(AttributeId::from_path("upstream.uri_san_peer_certificate").value());
    // v.push_back(AttributeId::from_path("upstream.local_address").value());
    // v.push_back(AttributeId::from_path("upstream.transport_failure_reason").value());

    // auto attrs = Attributes(*stream_info);
    // std::cerr << "StreamValues1\n";
    // auto value = attrs.buildAttributesValue(v);
    // std::cerr << "StreamValues2\n";
    // EXPECT_TRUE(value.has_map_value());
    // auto map_value = value.map_value();
  }
}
} // namespace
} // namespace Attributes
} // namespace Common
} // namespace Filters
} // namespace Extensions
} // namespace Envoy