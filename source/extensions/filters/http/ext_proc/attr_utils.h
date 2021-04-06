#pragma once

#include <atomic>
#include <map>
#include <memory>

#include "envoy/access_log/access_log.h"
#include "envoy/buffer/buffer.h"
#include "envoy/http/filter.h"
#include "envoy/stats/sink.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/common/assert.h"
#include "common/common/logger.h"
#include "absl/strings/string_view.h"

#include "include/envoy/stream_info/_virtual_includes/stream_info_interface/envoy/stream_info/stream_info.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

class AttrUtils: public Logger::Loggable<Logger::Id::filter> {
  public:
    AttrUtils(StreamInfo::StreamInfo &info, const google::protobuf::RepeatedPtrField<std::string> &specified, ProtobufWkt::Map<std::string, ProtobufWkt::Struct> &attributes): info_(info), specified_(specified), attributes_(attributes) {};
    ProtobufWkt::Map<std::string, ProtobufWkt::Struct>& build();

  private:
    void findValue(absl::string_view path);
    void requestSet(absl::string_view name);
    void responseSet(absl::string_view path);
    void connectionSet(absl::string_view path);
    void upstreamSet(absl::string_view path);
    void sourceSet(absl::string_view path);
    void destinationSet(absl::string_view path);
    void metadataSet();
    void filterStateSet();

    std::tuple<absl::string_view, absl::string_view> tokenizePath(absl::string_view path);
    ProtobufWkt::Map<std::string, ProtobufWkt::Value>& getOrInsert(std::string key);
    std::string getTs();

    StreamInfo::StreamInfo& info_;
    const google::protobuf::RepeatedPtrField<std::string>& specified_;
    ProtobufWkt::Map<std::string, ProtobufWkt::Struct>& attributes_;

};

} // ExternalProcessing
} // Extensions
} // HttpFilters
} // ExternalProcessing