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
  EXPECT_TRUE(value.has_map_value());
  auto map_value = value.map_value();
  EXPECT_EQ(0, map_value.entries_size());
}
TEST(Attributes, StreamValues) {
  auto stream_info = makeStreamInfo();
  auto attrs = Attributes(*stream_info);
  std::vector<AttributeId> v;
  auto value = attrs.buildAttributesValue(v);
  EXPECT_TRUE(value.has_map_value());
  auto map_value = value.map_value();
  EXPECT_EQ(0, map_value.entries_size());
}
} // namespace
} // namespace Attributes
} // namespace Common
} // namespace Filters
} // namespace Extensions
} // namespace Envoy