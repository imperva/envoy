#include <atomic>
#include <chrono>
#include <iterator>
#include <map>
#include <memory>
#include <string>

#include "absl/strings/str_format.h"

#include "envoy/access_log/access_log.h"
#include "envoy/buffer/buffer.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"
#include "envoy/stats/sink.h"
#include "envoy/upstream/cluster_manager.h"
#include "envoy/network/filter.h"

#include "source/common/grpc/common.h"
#include "source/common/http/headers.h"
#include "source/common/protobuf/utility.h"
#include "source/common/common/assert.h"
#include "source/common/common/logger.h"
#include "source/common/common/fmt.h"
#include "source/common/common/lock_guard.h"
#include "source/common/http/header_map_impl.h"

#include "source/extensions/filters/common/attributes/attributes.h"

namespace Envoy {
namespace Extensions {
namespace Filters {
namespace Common {
namespace Attributes {

Value ValueUtil::mapValue(MapValue* m) {
  Value val;
  val.set_allocated_map_value(m);
  return val;
}

Value ValueUtil::stringValue(const std::string& str) {
  Value val;
  val.set_string_value(str);
  return val;
}

Value ValueUtil::optionalStringValue(const absl::optional<std::string>& str) {
  if (str.has_value()) {
    return ValueUtil::stringValue(str.value());
  }
  return ValueUtil::nullValue();
}

Value ValueUtil::uint64Value(uint64_t n) {
  Value val;
  val.set_uint64_value(n);
  return val;
}

const Value ValueUtil::nullValue() {
  Value v1;
  Value v;
  v.set_null_value(v1.null_value());
  return v;
}

Http::RegisterCustomInlineHeader<Http::CustomInlineHeaderRegistry::Type::RequestHeaders>
    referer_handle = Http::CustomHeaders::get().Referer;

absl::optional<Value> Attributes::getAttribute(AttributeId& attr_id) {
  switch (attr_id.root()) {
  case RootToken::REQUEST:
    return getRequest(attr_id);
  case RootToken::RESPONSE:
    return getResponse(attr_id);
  case RootToken::SOURCE:
    return getSource(attr_id);
  case RootToken::DESTINATION:
    return getDestination(attr_id);
  case RootToken::CONNECTION:
    return getConnection(attr_id);
  case RootToken::UPSTREAM:
    return getUpstream(attr_id);
  case RootToken::METADATA:
    return getMetadata();
  case RootToken::FILTER_STATE:
    return getFilterState();
  }
  return absl::nullopt;
}

absl::optional<Value> Attributes::getRequest(AttributeId& attr_id) {
  RequestToken tok;
  bool valid = attr_id.sub(tok);
  if (!valid) {
    return absl::nullopt;
  }

  auto headers = request_headers_;

  switch (tok) {
  case RequestToken::PATH:
    if (headers != nullptr) {
      return ValueUtil::stringValue(std::string(headers->getPathValue()));
    }
    break;
  case RequestToken::URL_PATH: {
    if (headers != nullptr && headers->Path() != nullptr && headers->Path()->value() != nullptr) {
      auto path = headers->Path()->value().getStringView();
      auto end = std::max(path.find('\0'), path.find('?'));
      return ValueUtil::stringValue(std::string(path.substr(0, end)));
    }
    break;
  }
  case RequestToken::HOST:
    if (headers != nullptr) {
      return ValueUtil::stringValue(std::string(headers->getHostValue()));
    }
    break;
  case RequestToken::SCHEME:
    if (headers != nullptr) {
      return ValueUtil::stringValue(std::string(headers->getSchemeValue()));
    }
    break;
  case RequestToken::METHOD:
    if (headers != nullptr) {
      return ValueUtil::stringValue(std::string(headers->getMethodValue()));
    }
    break;
  case RequestToken::HEADERS:
    ENVOY_LOG(debug, "ignoring unimplemented attribute request.headers");
    break;
  case RequestToken::REFERER:
    if (headers != nullptr) {
      return ValueUtil::stringValue(std::string(headers->getInlineValue(referer_handle.handle())));
    }
    break;
  case RequestToken::USERAGENT:
    if (headers != nullptr) {
      return ValueUtil::stringValue(std::string(headers->getUserAgentValue()));
    }
    break;
  case RequestToken::TIME:
    return ValueUtil::stringValue(getTs());
    break;
  case RequestToken::ID:
    if (headers != nullptr) {
      return ValueUtil::stringValue(std::string(headers->getRequestIdValue()));
    }
    break;
  case RequestToken::PROTOCOL:
    return ValueUtil::optionalStringValue(
        HttpProtocolStrings[static_cast<int>(stream_info_.protocol().value())]);
  case RequestToken::DURATION:
    if (stream_info_.requestComplete().has_value()) {
      return ValueUtil::stringValue(
          formatDuration(absl::FromChrono(stream_info_.requestComplete().value())));
    }
    break;
  case RequestToken::SIZE:
    if (headers != nullptr && headers->ContentLength() != nullptr) {
      int64_t length;
      if (absl::SimpleAtoi(headers->ContentLength()->value().getStringView(), &length)) {
        return ValueUtil::uint64Value(length);
      }
    } else {
      return ValueUtil::uint64Value(stream_info_.bytesReceived());
    }
    break;
  case RequestToken::TOTAL_SIZE:
    return ValueUtil::uint64Value(
        stream_info_.bytesReceived() + headers != nullptr ? headers->byteSize() : 0);
  }
  return ValueUtil::nullValue();
}

absl::optional<Value> Attributes::getResponse(AttributeId& attr_id) {
  ResponseToken tok;
  bool valid = attr_id.sub(tok);
  if (!valid) {
    return absl::nullopt;
  }
  switch (tok) {
  case ResponseToken::CODE:
    if (stream_info_.responseCode().has_value()) {
      return ValueUtil::uint64Value(stream_info_.responseCode().value());
    }
    break;
  case ResponseToken::CODE_DETAILS:
    return ValueUtil::optionalStringValue(stream_info_.responseCodeDetails());
  case ResponseToken::FLAGS:
    return ValueUtil::uint64Value(stream_info_.responseFlags());
  case ResponseToken::GRPC_STATUS:
    ENVOY_LOG(debug, "ignoring unimplemented attribute response.grpc_status");
    break;
  case ResponseToken::HEADERS:
    ENVOY_LOG(debug, "ignoring unimplemented attribute response.headers");
    break;
  case ResponseToken::TRAILERS:
    ENVOY_LOG(debug, "ignoring unimplemented attribute response.trailers");
    break;
  case ResponseToken::SIZE:
    return ValueUtil::uint64Value(stream_info_.bytesSent());

  case ResponseToken::TOTAL_SIZE:
    return ValueUtil::uint64Value(stream_info_.bytesReceived());
  }
  return absl::nullopt;
}

absl::optional<Value> Attributes::getSource(AttributeId& attr_id) {
  SourceToken tok;
  bool valid = attr_id.sub(tok);
  if (!valid) {
    return absl::nullopt;
  }

  if (stream_info_.upstreamHost() == nullptr) {
    return absl::nullopt;
  }
  auto addr = stream_info_.upstreamHost()->address();
  if (addr == nullptr) {
    return ValueUtil::nullValue();
  }

  switch (tok) {
  case SourceToken::ADDRESS:
    return ValueUtil::stringValue(addr->asString());
  case SourceToken::PORT:
    if (addr->ip() == nullptr) {
      return ValueUtil::nullValue();
    }
    return ValueUtil::uint64Value(addr->ip()->port());
  }
  return absl::nullopt;
}

absl::optional<Value> Attributes::getDestination(AttributeId& attr_id) {
  DestinationToken tok;
  bool valid = attr_id.sub(tok);
  if (!valid) {
    return absl::nullopt;
  }

  auto addr = stream_info_.downstreamAddressProvider().localAddress();
  if (addr == nullptr) {
    return ValueUtil::nullValue();
  }

  switch (tok) {
  case DestinationToken::ADDRESS:
    return ValueUtil::stringValue(addr->asString());
  case DestinationToken::PORT:
    if (addr->ip() == nullptr) {
      return ValueUtil::nullValue();
    }
    return ValueUtil::uint64Value(addr->ip()->port());
  }
  return absl::nullopt;
}

absl::optional<Value> Attributes::getUpstream(AttributeId& attr_id) {
  UpstreamToken tok;
  bool valid = attr_id.sub(tok);
  if (!valid) {
    return absl::nullopt;
  }

  auto upstreamHost = stream_info_.upstreamHost();
  auto upstreamSsl = stream_info_.upstreamSslConnection();

  switch (tok) {
  case UpstreamToken::ADDRESS:
    if (upstreamHost != nullptr && upstreamHost->address() != nullptr) {
      return ValueUtil::stringValue(upstreamHost->address()->asString());
    }
    break;
  case UpstreamToken::PORT:
    if (upstreamHost != nullptr && upstreamHost->address() != nullptr &&
        upstreamHost->address()->ip() != nullptr) {
      return ValueUtil::uint64Value(upstreamHost->address()->ip()->port());
    }
    break;
  case UpstreamToken::TLS_VERSION:
    if (upstreamSsl != nullptr) {
      return ValueUtil::stringValue(upstreamSsl->tlsVersion());
    }
    break;
  case UpstreamToken::SUBJECT_LOCAL_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      return ValueUtil::stringValue(upstreamSsl->subjectLocalCertificate());
    }
    break;
  case UpstreamToken::SUBJECT_PEER_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      return ValueUtil::stringValue(upstreamSsl->subjectPeerCertificate());
    }
    break;
  case UpstreamToken::DNS_SAN_LOCAL_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      return ValueUtil::stringValue(upstreamSsl->dnsSansLocalCertificate().front());
    }
    break;
  case UpstreamToken::DNS_SAN_PEER_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      return ValueUtil::stringValue(upstreamSsl->dnsSansPeerCertificate().front());
    }
    break;
  case UpstreamToken::URI_SAN_LOCAL_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      return ValueUtil::stringValue(upstreamSsl->uriSanLocalCertificate().front());
    }
    break;
  case UpstreamToken::URI_SAN_PEER_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      return ValueUtil::stringValue(upstreamSsl->uriSanPeerCertificate().front());
    }
    break;
  case UpstreamToken::LOCAL_ADDRESS:
    if (stream_info_.upstreamLocalAddress() != nullptr) {
      return ValueUtil::stringValue(stream_info_.upstreamLocalAddress()->asString());
    }
    break;
  case UpstreamToken::TRANSPORT_FAILURE_REASON:
    return ValueUtil::stringValue(stream_info_.upstreamTransportFailureReason());
    break;
  }
  return absl::nullopt;
}

absl::optional<Value> Attributes::getConnection(AttributeId& attr_id) {
  ConnectionToken tok;
  bool valid = attr_id.sub(tok);
  if (!valid) {
    return absl::nullopt;
  }

  auto connId = stream_info_.connectionID();
  auto downstreamSsl = stream_info_.downstreamSslConnection();

  switch (tok) {
  case ConnectionToken::ID:
    if (connId.has_value()) {
      return ValueUtil::uint64Value(connId.value());
    }
    break;
  case ConnectionToken::MTLS:
    return absl::nullopt;
    if (downstreamSsl != nullptr) {
      return ValueUtil::boolValue(downstreamSsl->peerCertificatePresented());
    }
    break;
  case ConnectionToken::REQUESTED_SERVER_NAME:
    return ValueUtil::stringValue(stream_info_.requestedServerName());
  case ConnectionToken::TLS_VERSION:
    if (downstreamSsl != nullptr) {
      return ValueUtil::stringValue(downstreamSsl->tlsVersion());
    }
    break;
  case ConnectionToken::SUBJECT_LOCAL_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      return ValueUtil::stringValue(downstreamSsl->subjectLocalCertificate());
    }
    break;
  case ConnectionToken::SUBJECT_PEER_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      return ValueUtil::stringValue(downstreamSsl->subjectPeerCertificate());
    }
    break;
  case ConnectionToken::DNS_SAN_LOCAL_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      return ValueUtil::stringValue(std::string(downstreamSsl->dnsSansLocalCertificate().front()));
    }
    break;
  case ConnectionToken::DNS_SAN_PEER_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      return ValueUtil::stringValue(std::string(downstreamSsl->dnsSansPeerCertificate().front()));
    }
    break;
  case ConnectionToken::URI_SAN_LOCAL_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      return ValueUtil::stringValue(std::string(downstreamSsl->uriSanLocalCertificate().front()));
    }
    break;
  case ConnectionToken::URI_SAN_PEER_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      return ValueUtil::stringValue(std::string(downstreamSsl->uriSanPeerCertificate().front()));
    }
    break;
  case ConnectionToken::TERMINATION_DETAILS:
    if (downstreamSsl != nullptr) {
      return ValueUtil::optionalStringValue(stream_info_.connectionTerminationDetails());
    }
  }
  return absl::nullopt;
}

absl::optional<Value> Attributes::getMetadata() {
  //   if (attributes_.contains(METADATA_TOKEN)) {
  //     MapValue m;
  //     for (auto const& [k, v] : stream_info_.dynamicMetadata().filter_metadata()) {
  //       Value sk = ValueUtil::stringValue(k);
  //       // todo: convert s to object value (any)
  //       MapValue_Entry* e = m.add_entries();
  //       e->set_allocated_key(&sk);
  //       Value vv;
  //       Value any_v;
  //       Any* a = any_v.mutable_object_value();
  //       a->PackFrom(v);
  //       vv.set_allocated_object_value(a);
  //       e->set_allocated_value(&vv);
  //     }
  //     return absl::make_optional(ValueUtil::mapValue(&m));
  //   }
  return absl::nullopt;
}

// todo(eas): there are two issues here
// 1. FilterState seems to be an opaque data store and therefore does not have an iterator
// implemented on it,
//   it seems incorrect to just attach all of this data, need to check with envoy maintainers.
// 2. Encoding as a
// [`ProtobufWkt::Value`](https://developers.google.com/protocol-buffers/docs/reference/google.protobuf#value)
//   is problematic. In the [attribute
//   docs](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/advanced/attributes) it
//   is indicated that the filter state values should be binary data, but `Value` only allows the
//   following: [null, number, string, bool, struct, list] where struct is simply a `map<string,
//   value>`.
absl::optional<Value> Attributes::getFilterState() { return absl::nullopt; }

std::string Attributes::formatDuration(absl::Duration duration) {
  return absl::FormatDuration(duration);
}

std::string Attributes::getTs() {
  ProtobufWkt::Timestamp ts;
  TimestampUtil::systemClockToTimestamp(stream_info_.startTime(), ts);
  return Protobuf::util::TimeUtil::ToString(ts);
}

// ProtobufWkt::Map<std::string, ProtobufWkt::Value>& Attributes::getOrInsert(std::string key) {
//   if (attributes_.contains(key)) {
//     attributes_[key] = ProtobufWkt::Struct();
//   }
//   return *attributes_[key].mutable_fields();
// }

// todo(eas): this seems to result in a nullptr exception when empty headers are used.
// todo(eas): these are unavailable at headers time.
//
absl::optional<Value> Attributes::getGrpcStatus() {
  Http::ResponseHeaderMap& hs = response_headers_ != nullptr
                                    ? *response_headers_
                                    : *Envoy::Http::StaticEmptyHeaders::get().response_headers;

  Http::ResponseTrailerMap& ts = response_trailers_ != nullptr
                                     ? *response_trailers_
                                     : *Envoy::Http::StaticEmptyHeaders::get().response_trailers;

  if (!Envoy::Grpc::Common::hasGrpcContentType(hs)) {
    return absl::nullopt;
  }

  auto const& optional_status = Envoy::Grpc::Common::getGrpcStatus(ts, hs, stream_info_);

  if (optional_status.has_value()) {
    return ValueUtil::uint64Value(optional_status.value());
  }
  return absl::nullopt;
}
void Attributes::setRequestHeaders(Http::RequestHeaderMap* request_headers) {
  request_headers_ = request_headers;
}
void Attributes::setResponseHeaders(Http::ResponseHeaderMap* response_headers) {
  response_headers_ = response_headers;
}

} // namespace Attributes
} // namespace Common
} // namespace Filters
} // namespace Extensions
} // namespace Envoy