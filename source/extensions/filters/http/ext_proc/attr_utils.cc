#include <atomic>
#include <chrono>
#include <iterator>
#include <map>
#include <memory>
#include <string>

#include "common/grpc/common.h"
#include "common/http/headers.h"
#include "common/protobuf/utility.h"
#include "envoy/access_log/access_log.h"
#include "envoy/buffer/buffer.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"
#include "envoy/stats/sink.h"
#include "envoy/upstream/cluster_manager.h"
#include "envoy/network/filter.h"

#include "common/common/assert.h"
#include "common/common/logger.h"
#include "common/common/fmt.h"
#include "common/common/lock_guard.h"
#include "common/http/header_map_impl.h"

#include "extensions/filters/http/ext_proc/attr_utils.h"
#include "absl/strings/str_format.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

using google::api::expr::v1alpha1::Value;
using HashPolicy = envoy::config::route::v3::RouteAction::HashPolicy;
using AttrMap = ProtobufWkt::Map<std::string, ProtobufWkt::Value>;

static const std::string HttpProtocolStrings[] = {"Http 1.0", "Http 1.1", "Http 2", "Http 3"};

#define PROPERTY_TOKENS(_f)                                                                        \
  _f(METADATA) _f(REQUEST) _f(RESPONSE) _f(CONNECTION) _f(UPSTREAM) _f(SOURCE) _f(DESTINATION)     \
      _f(FILTER_STATE)

#define REQUEST_TOKENS(_f)                                                                         \
  _f(PATH) _f(URL_PATH) _f(HOST) _f(SCHEME) _f(METHOD) _f(HEADERS) _f(REFERER) _f(USERAGENT)       \
      _f(TIME) _f(ID) _f(PROTOCOL) _f(DURATION) _f(SIZE) _f(TOTAL_SIZE)

#define RESPONSE_TOKENS(_f)                                                                        \
  _f(CODE) _f(CODE_DETAILS) _f(FLAGS) _f(GRPC_STATUS) _f(HEADERS) _f(TRAILERS) _f(SIZE)            \
      _f(TOTAL_SIZE)

#define CONNECTION_TOKENS(_f)                                                                      \
  _f(ID) _f(MTLS) _f(REQUESTED_SERVER_NAME) _f(TLS_VERSION) _f(SUBJECT_LOCAL_CERTIFICATE)          \
      _f(SUBJECT_PEER_CERTIFICATE) _f(DNS_SAN_LOCAL_CERTIFICATE) _f(DNS_SAN_PEER_CERTIFICATE)      \
          _f(URI_SAN_LOCAL_CERTIFICATE) _f(URI_SAN_PEER_CERTIFICATE) _f(TERMINATION_DETAILS)

#define UPSTREAM_TOKENS(_f)                                                                        \
  _f(ADDRESS) _f(PORT) _f(TLS_VERSION) _f(SUBJECT_LOCAL_CERTIFICATE) _f(SUBJECT_PEER_CERTIFICATE)  \
      _f(DNS_SAN_LOCAL_CERTIFICATE) _f(DNS_SAN_PEER_CERTIFICATE) _f(URI_SAN_LOCAL_CERTIFICATE)     \
          _f(URI_SAN_PEER_CERTIFICATE) _f(LOCAL_ADDRESS) _f(TRANSPORT_FAILURE_REASON)

static inline std::string downCase(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

#define _DECLARE(_t) _t,
enum class PropertyToken { PROPERTY_TOKENS(_DECLARE) };
enum class RequestToken { REQUEST_TOKENS(_DECLARE) };
enum class ConnectionToken { CONNECTION_TOKENS(_DECLARE) };
enum class UpstreamToken { UPSTREAM_TOKENS(_DECLARE) };
enum class ResponseToken { RESPONSE_TOKENS(_DECLARE) };
#undef _DECLARE

#define _PAIR(_t) {downCase(#_t), PropertyToken::_t},
static absl::flat_hash_map<std::string, PropertyToken> property_tokens = {PROPERTY_TOKENS(_PAIR)};
#undef _PAIR

#define _PAIR(_t) {downCase(#_t), RequestToken::_t},
static absl::flat_hash_map<std::string, RequestToken> request_tokens = {REQUEST_TOKENS(_PAIR)};
#undef _PAIR

#define _PAIR(_t) {downCase(#_t), ResponseToken::_t},
static absl::flat_hash_map<std::string, ResponseToken> response_tokens = {RESPONSE_TOKENS(_PAIR)};
#undef _PAIR

#define _PAIR(_t) {downCase(#_t), ConnectionToken::_t},
static absl::flat_hash_map<std::string, ConnectionToken> connection_tokens = {
    CONNECTION_TOKENS(_PAIR)};
#undef _PAIR

#define _PAIR(_t) {downCase(#_t), UpstreamToken::_t},
static absl::flat_hash_map<std::string, UpstreamToken> upstream_tokens = {UPSTREAM_TOKENS(_PAIR)};
#undef _PAIR

const std::string REQUEST_TOKEN = "request";
const std::string RESPONSE_TOKEN = "response";
const std::string CONNECTION_TOKEN = "connection";
const std::string UPSTREAM_TOKEN = "upstream";
const std::string DOWNSTREAM_TOKEN = "downstream";
const std::string SOURCE_TOKEN = "source";
const std::string DESTINATION_TOKEN = "destination";
const std::string FILTER_STATE_TOKEN = "filter_state";
const std::string METADATA_TOKEN = "metadata";

const std::string METADATA_FILTER_METADATA_TOKEN = "metadata";

const std::string REQUEST_PATH_TOKEN = "path";
const std::string REQUEST_URL_PATH_TOKEN = "url_path";
const std::string REQUEST_HOST_TOKEN = "host";
const std::string REQUEST_SCHEME_TOKEN = "scheme";
const std::string REQUEST_METHOD_TOKEN = "method";
const std::string REQUEST_HEADERS_TOKEN = "headers";
const std::string REQUEST_REFERER_TOKEN = "referer";
const std::string REQUEST_USERAGENT_TOKEN = "useragent";
const std::string REQUEST_TIME_TOKEN = "time";
const std::string REQUEST_ID_TOKEN = "id";
const std::string REQUEST_PROTOCOL_TOKEN = "protocol";
const std::string REQUEST_DURATION_TOKEN = "duration";
const std::string REQUEST_SIZE_TOKEN = "size";
const std::string REQUEST_TOTAL_SIZE_TOKEN = "total_size";

const std::string RESPONSE_CODE_TOKEN = "code";
const std::string RESPONSE_CODE_DETAILS_TOKEN = "code_details";
const std::string RESPONSE_FLAGS_TOKEN = "flags";
const std::string RESPONSE_GRPC_STATUS_TOKEN = "grpc_status";
const std::string RESPONSE_HEADERS_TOKEN = "headers";
const std::string RESPONSE_TRAILERS_TOKEN = "trailers";
const std::string RESPONSE_SIZE_TOKEN = "size";
const std::string RESPONSE_TOTAL_SIZE_TOKEN = "total_size";

const std::string DESTINATION_ADDRESS_TOKEN = "address";
const std::string DESTINATION_PORT_TOKEN = "port";

const std::string SOURCE_ADDRESS_TOKEN = "address";
const std::string SOURCE_PORT_TOKEN = "port";

const std::string UPSTREAM_ADDRESS_TOKEN = "address";
const std::string UPSTREAM_PORT_TOKEN = "port";
const std::string UPSTREAM_TLS_VERSION_TOKEN = "tls_version";
const std::string UPSTREAM_SUBJECT_LOCAL_CERTIFICATE_TOKEN = "subject_local_certificate";
const std::string UPSTREAM_SUBJECT_PEER_CERTIFICATE_TOKEN = "subject_peer_certificate";
const std::string UPSTREAM_DNS_SAN_LOCAL_CERTIFICATE_TOKEN = "dns_san_local_certificate";
const std::string UPSTREAM_DNS_SAN_PEER_CERTIFICATE_TOKEN = "dns_san_peer_certificate";
const std::string UPSTREAM_URI_SAN_LOCAL_CERTIFICATE_TOKEN = "uri_san_local_certificate";
const std::string UPSTREAM_URI_SAN_PEER_CERTIFICATE_TOKEN = "uri_san_peer_certificate";
const std::string UPSTREAM_LOCAL_ADDRESS_TOKEN = "local_address";
const std::string UPSTREAM_TRANSPORT_FAILURE_REASON_TOKEN = "transport_failure_reason";

const std::string CONNECTION_ID_TOKEN = "id";
const std::string CONNECTION_MTLS_TOKEN = "mtls";
const std::string CONNECTION_TLS_VERSION_TOKEN = "tls_version";
const std::string CONNECTION_REQUESTED_SERVER_NAME_TOKEN = "requested_server_name";
const std::string CONNECTION_SUBJECT_LOCAL_CERTIFICATE_TOKEN = "subject_local_certificate";
const std::string CONNECTION_SUBJECT_PEER_CERTIFICATE_TOKEN = "subject_peer_certificate";
const std::string CONNECTION_DNS_SAN_LOCAL_CERTIFICATE_TOKEN = "dns_san_local_certificate";
const std::string CONNECTION_DNS_SAN_PEER_CERTIFICATE_TOKEN = "dns_san_peer_certificate";
const std::string CONNECTION_URI_SAN_LOCAL_CERTIFICATE_TOKEN = "uri_san_local_certificate";
const std::string CONNECTION_URI_SAN_PEER_CERTIFICATE_TOKEN = "uri_san_peer_certificate";
const std::string CONNECTION_TERMINATION_DETAILS_TOKEN = "termination_details";

Value ExprValueUtil::stringValue(const std::string& str) {
  Value val;
  val.set_string_value(str);
  return val;
}

Value ExprValueUtil::optionalStringValue(const absl::optional<std::string>& str) {
  if (str.has_value()) {
    return ExprValueUtil::stringValue(str.value());
  }
  return ExprValueUtil::nullValue();
}

const Value ExprValueUtil::nullValue() {
  static const auto* v = []() -> Value* {
    auto* vv = new Value();
    vv->set_null_value(ProtobufWkt::NULL_VALUE);
    return vv;
  }();
  return *v;
}
Http::RegisterCustomInlineHeader<Http::CustomInlineHeaderRegistry::Type::RequestHeaders>
    referer_handle = Http::CustomHeaders::get().Referer;

ProtobufWkt::Map<std::string, ProtobufWkt::Struct>& AttrUtils::build() {
  for (const std::string& s : specified_) {
    auto [root_tok, sub_tok] = tokenizePath(absl::string_view(s));
    findValue(root_tok, sub_tok);
  }
  return attributes_;
}

std::tuple<absl::string_view, absl::string_view> AttrUtils::tokenizePath(absl::string_view path) {
  // ex: "request.foobar"
  //             ^
  size_t root_token_str_end = std::min(path.find('.'), path.find('\0'));
  if (root_token_str_end == absl::string_view::npos) {
    root_token_str_end = path.size();
  }

  auto root_tok = path.substr(0, root_token_str_end);
  absl::string_view sub_tok;
  if (root_token_str_end + 1 < path.size()) {
    sub_tok = path.substr(root_token_str_end + 1,
                          std::min(path.find('\0'), path.size() - root_token_str_end - 1));
  }
  return std::tuple(root_tok, sub_tok);
}

Value AttrUtils::findValue(absl::string_view root_tok, absl::string_view sub_tok) {
  auto root_id = property_tokens.find(root_tok);
  if (root_id == property_tokens.end()) {
    ENVOY_LOG(debug, "The attribute '{}.{}' is not a valid ext_proc attribute", root_tok, sub_tok);
    return;
  }

  switch (root_id->second) {
  case PropertyToken::REQUEST:
    return AttrUtils::requestSet(sub_tok);
  case PropertyToken::RESPONSE:
    return AttrUtils::responseSet(sub_tok);
  case PropertyToken::CONNECTION:
    return AttrUtils::connectionSet(sub_tok);
  case PropertyToken::UPSTREAM:
    return AttrUtils::upstreamSet(sub_tok);
  case PropertyToken::SOURCE:
    return AttrUtils::sourceSet(sub_tok);
  case PropertyToken::DESTINATION:
    return AttrUtils::destinationSet(sub_tok);
  case PropertyToken::METADATA:
    if (sub_tok.empty()) {
      return AttrUtils::metadataSet();
    }
    return ExprValueUtil::nullValue();
  case PropertyToken::FILTER_STATE:
    if (sub_tok.empty()) {
      return AttrUtils::filterStateSet();
    }
    return ExprValueUtil::nullValue();
  }
}

Value AttrUtils::requestSet(absl::string_view path) {
  auto attr_fields = getOrInsert(REQUEST_TOKEN);

  auto part_token = request_tokens.find(path);
  if (part_token == request_tokens.end()) {
    ENVOY_LOG(debug, "Unable to find ext_proc request attribute: '{}'", path);
    return;
  }

  auto headers = request_headers_;
  int end;

  switch (part_token->second) {
  case RequestToken::PATH:
    if (headers != nullptr) {
      attr_fields[REQUEST_PATH_TOKEN] =
          ExprValueUtil::stringValue(std::string(headers->getPathValue()));
    }
    break;
  case RequestToken::URL_PATH:
    if (headers != nullptr && headers->Path() != nullptr && headers->Path()->value() != nullptr) {
      end = std::max(path.find('\0'), path.find('?'));
      attr_fields[REQUEST_URL_PATH_TOKEN] = ExprValueUtil::stringValue(
          std::string(headers->Path()->value().getStringView().substr(0, end)));
    }
    break;
  case RequestToken::HOST:
    if (headers != nullptr) {
      attr_fields[REQUEST_HOST_TOKEN] =
          ExprValueUtil::stringValue(std::string(headers->getHostValue()));
    }
    break;
  case RequestToken::SCHEME:
    if (headers != nullptr) {
      attr_fields[REQUEST_SCHEME_TOKEN] =
          ExprValueUtil::stringValue(std::string(headers->getSchemeValue()));
    }
    break;
  case RequestToken::METHOD:
    if (headers != nullptr) {
      attr_fields[REQUEST_METHOD_TOKEN] =
          ExprValueUtil::stringValue(std::string(headers->getMethodValue()));
    }
    break;
  case RequestToken::HEADERS:
    ENVOY_LOG(debug, "ignoring unimplemented attribute request.headers");
    break;
  case RequestToken::REFERER:
    if (headers != nullptr) {
      attr_fields[REQUEST_REFERER_TOKEN] =
          ExprValueUtil::stringValue(std::string(headers->getInlineValue(referer_handle.handle())));
    }
    break;
  case RequestToken::USERAGENT:
    if (headers != nullptr) {
      attr_fields[REQUEST_USERAGENT_TOKEN] =
          ExprValueUtil::stringValue(std::string(headers->getUserAgentValue()));
    }
    break;
  case RequestToken::TIME:
    attr_fields[REQUEST_TIME_TOKEN] = ExprValueUtil::stringValue(getTs());
    break;
  case RequestToken::ID:
    if (headers != nullptr) {
      attr_fields[REQUEST_ID_TOKEN] =
          ExprValueUtil::stringValue(std::string(headers->getRequestIdValue()));
    }
    break;
  case RequestToken::PROTOCOL:
    attr_fields[REQUEST_PROTOCOL_TOKEN] = ExprValueUtil::optionalStringValue(
        HttpProtocolStrings[static_cast<int>(info_.protocol().value())]);
    break;
  case RequestToken::DURATION:
    if (info_.requestComplete().has_value()) {
      attr_fields[REQUEST_DURATION_TOKEN] = ExprValueUtil::stringValue(
          formatDuration(absl::FromChrono(info_.requestComplete().value())));
    }
    break;
  case RequestToken::SIZE:
    if (headers != nullptr && headers->ContentLength() != nullptr) {
      int64_t length;
      if (absl::SimpleAtoi(headers->ContentLength()->value().getStringView(), &length)) {
        attr_fields[REQUEST_SIZE_TOKEN] = ExprValueUtil::int64Value(length);
      }
    } else {
      attr_fields[REQUEST_SIZE_TOKEN] = ExprValueUtil::uint64Value(info_.bytesReceived());
    }
    break;
  case RequestToken::TOTAL_SIZE:
    attr_fields[REQUEST_TOTAL_SIZE_TOKEN] = ExprValueUtil::uint64Value(
        info_.bytesReceived() + headers != nullptr ? headers->byteSize() : 0);
    break;
  }
}

Value AttrUtils::responseSet(absl::string_view path) {
  auto attr_fields = getOrInsert(RESPONSE_TOKEN);

  auto part_token = response_tokens.find(path);
  if (part_token == response_tokens.end()) {
    ENVOY_LOG(debug, "Unable to find ext_proc response attribute: '{}'", path);
    return;
  }

  switch (part_token->second) {
  case ResponseToken::CODE:
    if (info_.responseCode().has_value()) {
      attr_fields[RESPONSE_CODE_TOKEN] = ExprValueUtil::uint64Value(info_.responseCode().value());
    }
    break;
  case ResponseToken::CODE_DETAILS:
    attr_fields[RESPONSE_CODE_DETAILS_TOKEN] =
        ExprValueUtil::optionalStringValue(info_.responseCodeDetails());
    break;
  case ResponseToken::FLAGS:
    attr_fields[RESPONSE_FLAGS_TOKEN] = ExprValueUtil::uint64Value(info_.responseFlags());
    break;
  case ResponseToken::GRPC_STATUS:
    ENVOY_LOG(debug, "ignoring unimplemented attribute response.grpc_status");
    // attr_fields[RESPONSE_GRPC_STATUS_TOKEN] = getGrpcStatus();
    break;
  case ResponseToken::HEADERS:
    ENVOY_LOG(debug, "ignoring unimplemented attribute response.headers");
    break;
  case ResponseToken::TRAILERS:
    ENVOY_LOG(debug, "ignoring unimplemented attribute response.trailers");
    break;
  case ResponseToken::SIZE:
    attr_fields[RESPONSE_SIZE_TOKEN] = ExprValueUtil::uint64Value(info_.bytesSent());

  case ResponseToken::TOTAL_SIZE:
    attr_fields[RESPONSE_TOTAL_SIZE_TOKEN] = ExprValueUtil::uint64Value(info_.bytesReceived());
    break;
  }
}

Value AttrUtils::destinationSet(absl::string_view path) {
  auto attr_fields = getOrInsert(DESTINATION_TOKEN);

  auto addr = info_.downstreamAddressProvider().localAddress();
  if (addr == nullptr) {
    return;
  }
  if (path == DESTINATION_ADDRESS_TOKEN) {
    attr_fields[DESTINATION_ADDRESS_TOKEN] = ExprValueUtil::stringValue(addr->asString());
  } else if (path == DESTINATION_PORT_TOKEN) {
    if (addr->ip() == nullptr) {
      return;
    }
    attr_fields[DESTINATION_PORT_TOKEN] = ExprValueUtil::uint64Value(addr->ip()->port());
  } else {
    ENVOY_LOG(debug, "Unable to find ext_proc destination attribute: '{}'", path);
  }
}

Value AttrUtils::sourceSet(absl::string_view path) {
  if (!attributes_.contains(SOURCE_TOKEN)) {
    attributes_[SOURCE_TOKEN] = ProtobufWkt::Struct();
  }
  auto attr_fields = *attributes_[SOURCE_TOKEN].mutable_fields();

  if (info_.upstreamHost() == nullptr) {
    return;
  }
  auto addr = info_.upstreamHost()->address();
  if (addr == nullptr) {
    return;
  }
  if (path == SOURCE_ADDRESS_TOKEN) {
    attr_fields[SOURCE_ADDRESS_TOKEN] = ExprValueUtil::stringValue(addr->asString());
  } else if (path == SOURCE_PORT_TOKEN) {
    if (addr->ip() == nullptr) {
      return;
    }
    attr_fields[SOURCE_PORT_TOKEN] = ExprValueUtil::uint64Value(addr->ip()->port());
  } else {
    ENVOY_LOG(debug, "Unable to find ext_proc source attribute: '{}'", path);
  }
}

Value AttrUtils::upstreamSet(absl::string_view path) {
  auto attr_fields = getOrInsert(UPSTREAM_TOKEN);

  auto part_token = upstream_tokens.find(path);
  if (part_token == upstream_tokens.end()) {
    ENVOY_LOG(debug, "Unable to find ext_proc upstream attribute: '{}'", path);
    return;
  }

  auto upstreamHost = info_.upstreamHost();
  auto upstreamSsl = info_.upstreamSslConnection();
  switch (part_token->second) {
  case UpstreamToken::ADDRESS:
    if (upstreamHost != nullptr && upstreamHost->address() != nullptr) {
      attr_fields[UPSTREAM_ADDRESS_TOKEN] =
          ExprValueUtil::stringValue(upstreamHost->address()->asString());
    }
    break;
  case UpstreamToken::PORT:
    if (upstreamHost != nullptr && upstreamHost->address() != nullptr &&
        upstreamHost->address()->ip() != nullptr) {
      attr_fields[UPSTREAM_PORT_TOKEN] =
          ExprValueUtil::uint64Value(upstreamHost->address()->ip()->port());
    }
    break;
  case UpstreamToken::TLS_VERSION:
    if (upstreamSsl != nullptr) {
      attr_fields[UPSTREAM_TLS_VERSION_TOKEN] =
          ExprValueUtil::stringValue(upstreamSsl->tlsVersion());
    }
    break;
  case UpstreamToken::SUBJECT_LOCAL_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      attr_fields[UPSTREAM_SUBJECT_LOCAL_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(upstreamSsl->subjectLocalCertificate());
    }
    break;
  case UpstreamToken::SUBJECT_PEER_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      attr_fields[UPSTREAM_SUBJECT_LOCAL_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(upstreamSsl->subjectPeerCertificate());
    }
    break;
  case UpstreamToken::DNS_SAN_LOCAL_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      attr_fields[UPSTREAM_DNS_SAN_LOCAL_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(upstreamSsl->dnsSansLocalCertificate().front());
    }
    break;
  case UpstreamToken::DNS_SAN_PEER_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      attr_fields[UPSTREAM_DNS_SAN_PEER_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(upstreamSsl->dnsSansPeerCertificate().front());
    }
    break;
  case UpstreamToken::URI_SAN_LOCAL_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      attr_fields[UPSTREAM_URI_SAN_LOCAL_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(upstreamSsl->uriSanLocalCertificate().front());
    }
    break;
  case UpstreamToken::URI_SAN_PEER_CERTIFICATE:
    if (upstreamSsl != nullptr) {
      attr_fields[UPSTREAM_URI_SAN_PEER_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(upstreamSsl->uriSanPeerCertificate().front());
    }
    break;
  case UpstreamToken::LOCAL_ADDRESS:
    if (info_.upstreamLocalAddress() != nullptr) {
      attr_fields[UPSTREAM_LOCAL_ADDRESS_TOKEN] =
          ExprValueUtil::stringValue(info_.upstreamLocalAddress()->asString());
    }
    break;
  case UpstreamToken::TRANSPORT_FAILURE_REASON:
    attr_fields[UPSTREAM_TRANSPORT_FAILURE_REASON_TOKEN] =
        ExprValueUtil::stringValue(info_.upstreamTransportFailureReason());
    break;
  }
}

Value AttrUtils::connectionSet(absl::string_view path) {
  auto attr_fields = getOrInsert(CONNECTION_TOKEN);

  auto part_token = connection_tokens.find(path);
  if (part_token == connection_tokens.end()) {
    ENVOY_LOG(debug, "Unable to find ext_proc connection attribute: '{}'", path);
    return;
  }

  auto connId = info_.connectionID();
  auto downstreamSsl = info_.downstreamSslConnection();

  switch (part_token->second) {
  case ConnectionToken::ID:
    if (connId.has_value()) {
      attr_fields[CONNECTION_ID_TOKEN] = ExprValueUtil::uint64Value(connId.value());
    }
    break;
  case ConnectionToken::MTLS:
    return;
    if (downstreamSsl != nullptr) {
      attr_fields[CONNECTION_MTLS_TOKEN] =
          ExprValueUtil::boolValue(downstreamSsl->peerCertificatePresented());
    }
    break;
  case ConnectionToken::REQUESTED_SERVER_NAME:
    attr_fields[CONNECTION_REQUESTED_SERVER_NAME_TOKEN] =
        ExprValueUtil::stringValue(info_.requestedServerName());
    break;
  case ConnectionToken::TLS_VERSION:
    if (downstreamSsl != nullptr) {
      attr_fields[CONNECTION_TLS_VERSION_TOKEN] =
          ExprValueUtil::stringValue(downstreamSsl->tlsVersion());
    }
    break;
  case ConnectionToken::SUBJECT_LOCAL_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      attr_fields[CONNECTION_SUBJECT_LOCAL_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(downstreamSsl->subjectLocalCertificate());
    }
    break;
  case ConnectionToken::SUBJECT_PEER_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      attr_fields[CONNECTION_SUBJECT_PEER_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(downstreamSsl->subjectPeerCertificate());
    }
    break;
  case ConnectionToken::DNS_SAN_LOCAL_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      attr_fields[CONNECTION_DNS_SAN_LOCAL_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(std::string(downstreamSsl->dnsSansLocalCertificate().front()));
    }
    break;
  case ConnectionToken::DNS_SAN_PEER_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      attr_fields[CONNECTION_DNS_SAN_PEER_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(std::string(downstreamSsl->dnsSansPeerCertificate().front()));
    }
    break;
  case ConnectionToken::URI_SAN_LOCAL_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      attr_fields[CONNECTION_URI_SAN_LOCAL_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(std::string(downstreamSsl->uriSanLocalCertificate().front()));
    }
    break;
  case ConnectionToken::URI_SAN_PEER_CERTIFICATE:
    if (downstreamSsl != nullptr) {
      attr_fields[CONNECTION_URI_SAN_PEER_CERTIFICATE_TOKEN] =
          ExprValueUtil::stringValue(std::string(downstreamSsl->uriSanPeerCertificate().front()));
    }
    break;
  case ConnectionToken::TERMINATION_DETAILS:
    if (downstreamSsl != nullptr) {
      attr_fields[CONNECTION_TERMINATION_DETAILS_TOKEN] =
          ExprValueUtil::optionalStringValue(info_.connectionTerminationDetails());
    }
  }
}

Value AttrUtils::metadataSet() {
  if (!attributes_.contains(METADATA_TOKEN)) {
    attributes_[METADATA_TOKEN] = ProtobufWkt::Struct();
    (*attributes_[METADATA_TOKEN].mutable_fields())[METADATA_FILTER_METADATA_TOKEN] =
        ExprValueUtil::structValue(ProtobufWkt::Struct());
    for (auto const& [k, s] : info_.dynamicMetadata().filter_metadata()) {
      (*((*attributes_[METADATA_TOKEN].mutable_fields())[METADATA_FILTER_METADATA_TOKEN]
             .mutable_struct_value())
            ->mutable_fields())[k] = ExprValueUtil::structValue(s);
    }
  }
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
void AttrUtils::filterStateSet() {
  ENVOY_LOG(debug, "ignoring unimplemented attribute filter_state");
}

std::string AttrUtils::formatDuration(absl::Duration duration) {
  return absl::FormatDuration(duration);
}

std::string AttrUtils::getTs() {
  ProtobufWkt::Timestamp ts;
  TimestampUtil::systemClockToTimestamp(info_.startTime(), ts);
  return Protobuf::util::TimeUtil::ToString(ts);
}

ProtobufWkt::Map<std::string, ProtobufWkt::Value>& AttrUtils::getOrInsert(std::string key) {
  if (attributes_.contains(key)) {
    attributes_[key] = ProtobufWkt::Struct();
  }
  return *attributes_[key].mutable_fields();
}

// todo(eas): this seems to result in a nullptr exception when empty headers are used.
ProtobufWkt::Value AttrUtils::getGrpcStatus() {
  Http::ResponseHeaderMap& hs = response_headers_ != nullptr
                                    ? *response_headers_
                                    : *Envoy::Http::StaticEmptyHeaders::get().response_headers;
  Http::ResponseTrailerMap& ts = response_trailers_ != nullptr
                                     ? *response_trailers_
                                     : *Envoy::Http::StaticEmptyHeaders::get().response_trailers;

  if (!Envoy::Grpc::Common::hasGrpcContentType(hs)) {
    return ExprValueUtil::nullValue();
  }

  auto const& optional_status = Envoy::Grpc::Common::getGrpcStatus(ts, hs, info_);

  if (optional_status.has_value()) {
    return ExprValueUtil::uint64Value(optional_status.value());
  }
  return ExprValueUtil::nullValue();
}
void AttrUtils::setRequestHeaders(Http::RequestHeaderMap* request_headers) {
  request_headers_ = request_headers;
}
void AttrUtils::setRequestTrailers(Http::RequestTrailerMap* request_trailers) {
  request_trailers_ = request_trailers;
}
void AttrUtils::setResponseHeaders(Http::ResponseHeaderMap* response_headers) {
  response_headers_ = response_headers;
}
void AttrUtils::setResponseTrailers(Http::ResponseTrailerMap* response_trailers) {
  response_trailers_ = response_trailers;
}

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy