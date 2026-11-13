#include "connectrpc/unary.h"

#include <algorithm>
#include <charconv>
#include <utility>

#include "absl/strings/escaping.h"
#include "get_internal.h"
#include "google/protobuf/struct.pb.h"
#include "google/protobuf/util/json_util.h"
#include "protocol_internal.h"

namespace connectrpc {
namespace internal_protocol {
constexpr std::string_view kCodes[] = {"",
                                       "canceled",
                                       "unknown",
                                       "invalid_argument",
                                       "deadline_exceeded",
                                       "not_found",
                                       "already_exists",
                                       "permission_denied",
                                       "resource_exhausted",
                                       "failed_precondition",
                                       "aborted",
                                       "out_of_range",
                                       "unimplemented",
                                       "internal",
                                       "unavailable",
                                       "data_loss",
                                       "unauthenticated"};
constexpr int kHttpCodes[] = {200, 499, 500, 400, 504, 404, 409, 403, 429,
                              400, 409, 400, 501, 500, 503, 500, 401};
Status Invalid(std::string message) {
  return {Code::kInvalidArgument, std::move(message), {}};
}
std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    text.remove_suffix(1);
  return text;
}
std::string Lower(std::string_view text) {
  std::string result(text);
  for (char& c : result)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return result;
}
Result<std::string> Single(const Metadata& headers, std::string_view key) {
  std::string value;
  bool seen = false;
  for (const auto& [name, entry] : headers) {
    if (name != key) continue;
    if (seen)
      return Result<std::string>(Invalid("duplicate " + std::string(key)));
    value = entry;
    seen = true;
  }
  return Result<std::string>(std::move(value));
}
Result<Codec> ContentType(std::string_view input) {
  std::string_view type = Trim(input.substr(0, input.find(';')));
  std::string media = Lower(type);
  Codec codec;
  if (media == "application/json")
    codec = Codec::kJson;
  else if (media == "application/proto")
    codec = Codec::kProtobuf;
  else
    return Result<Codec>(
        {Code::kUnimplemented, "unsupported content type", {}});
  size_t offset = input.find(';');
  while (offset != std::string_view::npos) {
    input.remove_prefix(offset + 1);
    offset = std::string_view::npos;
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < input.size(); ++i) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (quoted && input[i] == '\\') {
        escaped = true;
        continue;
      }
      if (input[i] == '"') quoted = !quoted;
      if (input[i] == ';' && !quoted) {
        offset = i;
        break;
      }
    }
    std::string_view parameter = Trim(input.substr(0, offset));
    size_t equal = parameter.find('=');
    if (equal == std::string_view::npos) continue;
    if (Lower(Trim(parameter.substr(0, equal))) != "charset" ||
        codec != Codec::kJson)
      continue;
    std::string_view value = Trim(parameter.substr(equal + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value.remove_prefix(1);
      value.remove_suffix(1);
    }
    if (Lower(value) != "utf-8")
      return Result<Codec>(
          {Code::kUnimplemented, "unsupported JSON charset", {}});
  }
  return Result<Codec>(codec);
}
std::string MediaType(Codec codec) {
  return codec == Codec::kJson ? "application/json" : "application/proto";
}
bool Reserved(std::string_view name) {
  return name.empty() || name.front() == ':' || name.starts_with("connect-") ||
         name.starts_with("grpc-") || name.starts_with("trailer-") ||
         name == "content-type" || name == "content-length" ||
         name == "content-encoding" || name == "accept-encoding" ||
         name == "host" || name == "connection" || name == "te";
}
Status ValidateMetadata(const Metadata& metadata) {
  for (const auto& [name, value] : metadata) {
    if (Reserved(name)) return Invalid("reserved metadata key: " + name);
    for (char c : name) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.')) {
        return Invalid("invalid metadata key");
      }
    }
    for (unsigned char c : value)
      if (c < 32 || c > 126) return Invalid("non-ASCII metadata value");
    if (name.ends_with("-bin")) {
      std::string decoded;
      if (!absl::Base64Unescape(value, &decoded))
        return Invalid("invalid binary metadata");
    }
  }
  return {};
}
Result<std::string> Body(HttpMessage& message, size_t limit) {
  if (message.body.size() > limit)
    return Result<std::string>(
        {Code::kResourceExhausted, "body exceeds limit", {}});
  auto encoding = Single(message.headers, "content-encoding");
  if (!encoding.ok()) return encoding;
  if (encoding.value()->empty() || *encoding.value() == "identity")
    return Result<std::string>(std::move(message.body));
  if (*encoding.value() != "gzip")
    return Result<std::string>(
        {Code::kUnimplemented, "supported encodings: identity, gzip", {}});
  if (message.body.empty()) return Result<std::string>(std::string());
  return Gunzip(message.body, limit);
}
void ExtractMetadata(const Metadata& wire, Metadata& headers,
                     Metadata& trailers) {
  for (const auto& [name, value] : wire) {
    if (name.starts_with("trailer-"))
      trailers.emplace_back(name.substr(8), value);
    else if (!Reserved(name))
      headers.emplace_back(name, value);
  }
}
bool AcceptsGzip(std::string_view value) {
  while (!value.empty()) {
    size_t comma = value.find(',');
    if (Trim(value.substr(0, comma)) == "gzip") return true;
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
  }
  return false;
}
bool PrefersGzip(std::string_view value) {
  while (!value.empty()) {
    size_t comma = value.find(',');
    std::string_view coding = Trim(value.substr(0, comma));
    if (coding == "identity") return false;
    if (coding == "gzip") return true;
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
  }
  return false;
}
}  // namespace internal_protocol
namespace {
using internal_protocol::Body;
using internal_protocol::ContentType;
using internal_protocol::ExtractMetadata;
using internal_protocol::Invalid;
using internal_protocol::kCodes;
using internal_protocol::kHttpCodes;
using internal_protocol::Lower;
using internal_protocol::MediaType;
using internal_protocol::PrefersGzip;
using internal_protocol::Single;
using internal_protocol::Trim;
using internal_protocol::ValidateMetadata;
}  // namespace
Result<Codec> ParseUnaryContentType(std::string_view value) {
  return ContentType(value);
}
std::string_view CodeName(Code code) {
  int index = static_cast<int>(code);
  return index > 0 && index < 17 ? kCodes[index] : "unknown";
}
int HttpStatus(Code code) {
  int index = static_cast<int>(code);
  return index >= 0 && index < 17 ? kHttpCodes[index] : 500;
}
Code CodeFromHttpStatus(int status) {
  switch (status) {
    case 400:
      return Code::kInternal;
    case 401:
      return Code::kUnauthenticated;
    case 403:
      return Code::kPermissionDenied;
    case 404:
      return Code::kUnimplemented;
    case 429:
    case 502:
    case 503:
    case 504:
      return Code::kUnavailable;
    default:
      return Code::kUnknown;
  }
}
Result<std::string> EncodeError(const Status& status) {
  google::protobuf::Struct object;
  auto& fields = *object.mutable_fields();
  fields["code"].set_string_value(std::string(CodeName(status.code)));
  if (!status.message.empty())
    fields["message"].set_string_value(status.message);
  for (const auto& detail : status.details) {
    auto& entry = *fields["details"]
                       .mutable_list_value()
                       ->add_values()
                       ->mutable_struct_value()
                       ->mutable_fields();
    entry["type"].set_string_value(detail.type);
    std::string encoded = absl::Base64Escape(detail.value);
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    entry["value"].set_string_value(encoded);
  }
  return EncodeMessage(object, Codec::kJson);
}
Result<DecodedError> DecodeError(std::string_view json, Code fallback_code) {
  google::protobuf::Struct object;
  if (!DecodeMessage(json, Codec::kJson, object).ok())
    return Result<DecodedError>(Invalid("malformed error JSON"));
  const auto& fields = object.fields();
  auto code = fields.find("code");
  Status status{fallback_code, "", {}};
  if (code != fields.end() &&
      code->second.kind_case() != google::protobuf::Value::kNullValue) {
    if (code->second.kind_case() != google::protobuf::Value::kStringValue)
      return Result<DecodedError>(Invalid("invalid error code"));
    for (int i = 1; i < 17; ++i)
      if (code->second.string_value() == kCodes[i])
        status.code = static_cast<Code>(i);
  }
  auto message = fields.find("message");
  if (message != fields.end()) {
    if (message->second.kind_case() != google::protobuf::Value::kStringValue)
      return Result<DecodedError>(Invalid("invalid error message"));
    status.message = message->second.string_value();
  }
  auto details = fields.find("details");
  if (details != fields.end()) {
    if (details->second.kind_case() != google::protobuf::Value::kListValue)
      return Result<DecodedError>(Invalid("invalid error details"));
    for (const auto& item : details->second.list_value().values()) {
      if (item.kind_case() != google::protobuf::Value::kStructValue)
        return Result<DecodedError>(Invalid("invalid error detail"));
      const auto& entry = item.struct_value().fields();
      auto type = entry.find("type");
      auto value = entry.find("value");
      if (type == entry.end() || value == entry.end() ||
          type->second.kind_case() != google::protobuf::Value::kStringValue ||
          value->second.kind_case() != google::protobuf::Value::kStringValue)
        return Result<DecodedError>(Invalid("invalid error detail fields"));
      ErrorDetail detail{type->second.string_value(), ""};
      if (!absl::Base64Unescape(value->second.string_value(), &detail.value))
        return Result<DecodedError>(Invalid("invalid error detail base64"));
      status.details.push_back(std::move(detail));
    }
  }
  return Result<DecodedError>(DecodedError{std::move(status)});
}
Result<HttpMessage> EncodeUnaryRequest(std::string procedure,
                                       std::string payload,
                                       const CallOptions& options,
                                       MethodOptions method) {
  if (options.protocol != Protocol::kConnect || options.grpc_web_text)
    return Result<HttpMessage>(
        Invalid("unary body encoding requires Connect protocol"));
  if (options.use_get && method.idempotency != Idempotency::kNoSideEffects)
    return Result<HttpMessage>(
        Invalid("GET requires a side-effect-free method"));
  if (procedure.empty() || procedure.front() != '/' ||
      procedure.find_first_of("?#\r\n ") != std::string::npos)
    return Result<HttpMessage>(Invalid("invalid procedure path"));
  Status status = ValidateMetadata(options.headers);
  if (!status.ok()) return Result<HttpMessage>(status);
  HttpMessage request{0,
                      {{":method", "POST"},
                       {":path", std::move(procedure)},
                       {"content-type", MediaType(options.codec)},
                       {"connect-protocol-version", "1"},
                       {"accept-encoding", "gzip"}},
                      std::move(payload)};
  if (request.body.size() > options.transport.max_body_bytes)
    return Result<HttpMessage>(
        {Code::kResourceExhausted, "message exceeds limit", {}});
  if (options.transport.deadline !=
      std::chrono::steady_clock::time_point::max()) {
    auto remaining =
        options.transport.deadline - std::chrono::steady_clock::now();
    if (remaining <= remaining.zero())
      return Result<HttpMessage>(
          {Code::kDeadlineExceeded, "deadline exceeded", {}});
    auto milliseconds =
        std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    request.headers.emplace_back(
        "connect-timeout-ms",
        std::to_string(std::min<int64_t>(milliseconds, 9999999999)));
  }
  if (options.compress) {
    auto compressed = Gzip(request.body);
    if (!compressed.ok()) return Result<HttpMessage>(compressed.status());
    request.body = std::move(*compressed.value());
    request.headers.emplace_back("content-encoding", "gzip");
  }
  request.headers.insert(request.headers.end(), options.headers.begin(),
                         options.headers.end());
  if (options.use_get) {
    Status encoded = internal_get::Encode(request, options.compress,
                                          options.transport.max_metadata_bytes);
    if (!encoded.ok()) return Result<HttpMessage>(encoded);
  }
  return Result<HttpMessage>(std::move(request));
}
Result<UnaryRequest> DecodeUnaryRequest(HttpMessage request, size_t max_bytes) {
  auto verb = Single(request.headers, ":method");
  if (!verb.ok()) return Result<UnaryRequest>(verb.status());
  if (*verb.value() == "GET") {
    auto normalized = internal_get::Decode(std::move(request), max_bytes);
    if (!normalized.status.ok()) return Result<UnaryRequest>(normalized.status);
    auto decoded = DecodeUnaryRequest(std::move(normalized.message), max_bytes);
    if (decoded.ok()) decoded.value()->is_get = true;
    return decoded;
  }
  for (const auto& [name, value] : request.headers) {
    if (name == "connect-timeout-ms" && value.empty()) {
      return Result<UnaryRequest>(Invalid("empty timeout"));
    }
  }
  auto method = Single(request.headers, ":method");
  auto path = Single(request.headers, ":path");
  auto type = Single(request.headers, "content-type");
  auto version = Single(request.headers, "connect-protocol-version");
  auto timeout = Single(request.headers, "connect-timeout-ms");
  for (const auto* field : {&method, &path, &type, &version, &timeout})
    if (!field->ok()) return Result<UnaryRequest>(field->status());
  if (*method.value() != "POST")
    return Result<UnaryRequest>(
        {Code::kUnimplemented, "method must be POST", {}});
  if (!version.value()->empty() && *version.value() != "1")
    return Result<UnaryRequest>(Invalid("unsupported protocol version"));
  auto codec = ContentType(*type.value());
  if (!codec.ok()) return Result<UnaryRequest>(codec.status());
  UnaryRequest decoded;
  decoded.procedure = std::move(*path.value());
  decoded.codec = *codec.value();
  if (!timeout.value()->empty()) {
    int64_t milliseconds = 0;
    const std::string& text = *timeout.value();
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), milliseconds);
    if (text.size() > 10 || text.front() < '0' || text.front() > '9' ||
        error != std::errc() || end != text.data() + text.size() ||
        milliseconds < 0)
      return Result<UnaryRequest>(Invalid("invalid timeout"));
    if (milliseconds == 0)
      return Result<UnaryRequest>(
          {Code::kDeadlineExceeded, "deadline exceeded", {}});
    decoded.deadline = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(milliseconds);
  }
  auto accept = Single(request.headers, "accept-encoding");
  if (!accept.ok()) return Result<UnaryRequest>(accept.status());
  if (accept.value()->empty())
    accept = Single(request.headers, "content-encoding");
  if (!accept.ok()) return Result<UnaryRequest>(accept.status());
  decoded.accept_gzip = PrefersGzip(*accept.value());
  Metadata ignored;
  ExtractMetadata(request.headers, decoded.headers, ignored);
  auto body = Body(request, max_bytes);
  if (!body.ok()) return Result<UnaryRequest>(body.status());
  decoded.payload = std::move(*body.value());
  return Result<UnaryRequest>(std::move(decoded));
}
Result<HttpMessage> EncodeUnaryResponse(UnaryResponse response, Codec codec,
                                        bool compress) {
  Status status = ValidateMetadata(response.headers);
  if (status.ok()) status = ValidateMetadata(response.trailers);
  if (!status.ok()) return Result<HttpMessage>(status);
  HttpMessage message{
      0,
      {{":status", std::to_string(HttpStatus(response.status.code))},
       {"content-type",
        response.status.ok() ? MediaType(codec) : "application/json"}},
      std::move(response.payload)};
  if (!response.status.ok()) {
    auto error = EncodeError(response.status);
    if (!error.ok()) return Result<HttpMessage>(error.status());
    message.body = std::move(*error.value());
  }
  if (compress && !message.body.empty()) {
    auto compressed = Gzip(message.body);
    if (!compressed.ok()) return Result<HttpMessage>(compressed.status());
    message.body = std::move(*compressed.value());
    message.headers.emplace_back("content-encoding", "gzip");
  }
  message.headers.insert(message.headers.end(), response.headers.begin(),
                         response.headers.end());
  for (auto& [name, value] : response.trailers)
    message.headers.emplace_back("trailer-" + name, std::move(value));
  return Result<HttpMessage>(std::move(message));
}
UnaryResponse DecodeUnaryResponse(HttpMessage response, Codec codec,
                                  size_t max_bytes) {
  UnaryResponse decoded;
  ExtractMetadata(response.headers, decoded.headers, decoded.trailers);
  auto code = Single(response.headers, ":status");
  int http_status = 0;
  if (code.ok()) {
    const std::string& text = *code.value();
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), http_status);
    if (error != std::errc() || end != text.data() + text.size() ||
        text.size() != 3)
      http_status = 0;
  }
  if (http_status < 100 || http_status > 999) {
    decoded.status = {Code::kInternal, "invalid HTTP status", {}};
    return decoded;
  }
  auto body = Body(response, max_bytes);
  if (!body.ok()) {
    decoded.status = body.status();
    if (decoded.status.code == Code::kUnimplemented)
      decoded.status.code = Code::kInternal;
    return decoded;
  }
  auto type = Single(response.headers, "content-type");
  if (http_status != 200) {
    decoded.status = {CodeFromHttpStatus(http_status),
                      "HTTP " + std::to_string(http_status),
                      {}};
    if (type.ok()) {
      auto parsed_type = ContentType(*type.value());
      if (parsed_type.ok() && *parsed_type.value() == Codec::kJson) {
        auto error =
            DecodeError(*body.value(), CodeFromHttpStatus(http_status));
        if (error.ok()) decoded.status = std::move(error.value()->status);
      }
    }
    return decoded;
  }
  auto parsed_type =
      type.ok() ? ContentType(*type.value()) : Result<Codec>(type.status());
  if (!parsed_type.ok() || *parsed_type.value() != codec) {
    // A Connect-looking media type with an unsupported codec/parameter is a
    // protocol error. An unrelated media type is an unknown HTTP response.
    std::string media = type.ok() ? Lower(Trim(*type.value())) : "";
    Code error_code =
        media.starts_with("application/") ? Code::kInternal : Code::kUnknown;
    decoded.status = {error_code, "unexpected response content type", {}};
    return decoded;
  }
  decoded.payload = std::move(*body.value());
  return decoded;
}
}  // namespace connectrpc
