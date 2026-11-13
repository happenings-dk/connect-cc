#include "connectrpc/streaming_protocol.h"

#include <utility>

#include "connectrpc/grpc_protocol.h"
#include "connectrpc/grpc_web_protocol.h"
#include "connectrpc/unary.h"
#include "google/protobuf/struct.pb.h"
#include "protocol_internal.h"

namespace connectrpc {
namespace {
using internal_protocol::Lower;
using internal_protocol::Single;
using internal_protocol::Trim;
Status Invalid(std::string message) {
  return {Code::kInvalidArgument, std::move(message), {}};
}
Status TooLarge() {
  return {
      Code::kResourceExhausted, "stream message or metadata exceeds limit", {}};
}
std::string ContentType(Codec codec) {
  return codec == Codec::kJson ? "application/connect+json"
                               : "application/connect+proto";
}
Result<bool> Compression(const Metadata& headers) {
  auto encoding = Single(headers, "connect-content-encoding");
  if (!encoding.ok()) return Result<bool>(encoding.status());
  if (encoding.value()->empty() || *encoding.value() == "identity")
    return Result<bool>(false);
  if (*encoding.value() == "gzip") return Result<bool>(true);
  return Result<bool>(
      {Code::kUnimplemented, "supported encodings: identity, gzip", {}});
}
Status MetadataSize(const Metadata& metadata, size_t limit) {
  size_t remaining = limit;
  for (const auto& [name, value] : metadata) {
    for (size_t size : {name.size(), value.size(), size_t{32}}) {
      if (size > remaining) return TooLarge();
      remaining -= size;
    }
  }
  return {};
}
}  // namespace
Result<Codec> ParseStreamingContentType(std::string_view type) {
  std::string normalized = Lower(Trim(type));
  constexpr std::string_view kPrefix = "application/connect+";
  if (!normalized.starts_with(kPrefix))
    return Result<Codec>(
        {Code::kUnimplemented, "unsupported streaming content type", {}});
  return ParseUnaryContentType("application/" +
                               normalized.substr(kPrefix.size()));
}
Result<Metadata> EncodeStreamingRequest(const StreamingRequest& request) {
  if (request.protocol != Protocol::kGrpcWeb &&
      (request.text || request.accept_text))
    return Result<Metadata>(Invalid("text encoding requires gRPC-Web"));
  if (request.protocol == Protocol::kGrpcWeb)
    return EncodeGrpcWebRequest(request);
  if (request.protocol == Protocol::kGrpc) return EncodeGrpcRequest(request);
  if (request.protocol != Protocol::kConnect)
    return Result<Metadata>(
        {Code::kUnimplemented, "protocol not implemented", {}});
  // Share method, path, timeout, and custom metadata rules with unary Connect.
  // Compression belongs to envelopes and is deliberately absent from this body.
  CallOptions options;
  options.codec = request.codec;
  options.headers = request.headers;
  options.transport.deadline = request.deadline;
  auto base = EncodeUnaryRequest(request.procedure, "", options);
  if (!base.ok()) return Result<Metadata>(base.status());
  Metadata headers;
  for (auto& [name, value] : base.value()->headers) {
    if (name == "content-type") value = ContentType(request.codec);
    if (name == "accept-encoding") {
      name = "connect-accept-encoding";
      value = request.accept_gzip ? "gzip" : "identity";
    }
    headers.emplace_back(std::move(name), std::move(value));
  }
  if (request.gzip) headers.emplace_back("connect-content-encoding", "gzip");
  return Result<Metadata>(std::move(headers));
}
Result<StreamingRequest> DecodeStreamingRequest(const Metadata& headers) {
  auto type = Single(headers, "content-type");
  if (!type.ok()) return Result<StreamingRequest>(type.status());
  if (ParseGrpcWebContentType(*type.value()).ok())
    return DecodeGrpcWebRequest(headers);
  if (ParseGrpcContentType(*type.value()).ok())
    return DecodeGrpcRequest(headers);
  auto codec = ParseStreamingContentType(*type.value());
  if (!codec.ok()) return Result<StreamingRequest>(codec.status());
  auto gzip = Compression(headers);
  if (!gzip.ok()) return Result<StreamingRequest>(gzip.status());
  auto accept = Single(headers, "connect-accept-encoding");
  if (!accept.ok()) return Result<StreamingRequest>(accept.status());
  if (accept.value()->empty())
    accept = Single(headers, "connect-content-encoding");
  if (!accept.ok()) return Result<StreamingRequest>(accept.status());
  HttpMessage base;
  for (const auto& [name, value] : headers) {
    // HTTP content coding is unrelated to per-message compression.
    if (name == "content-encoding" || name == "accept-encoding") continue;
    base.headers.emplace_back(
        name, name == "content-type"
                  ? (*codec.value() == Codec::kJson ? "application/json"
                                                    : "application/proto")
                  : value);
  }
  auto decoded = DecodeUnaryRequest(std::move(base), 0);
  if (!decoded.ok()) return Result<StreamingRequest>(decoded.status());
  StreamingRequest request;
  request.procedure = std::move(decoded.value()->procedure);
  request.codec = *codec.value();
  request.headers = std::move(decoded.value()->headers);
  request.deadline = decoded.value()->deadline;
  request.gzip = *gzip.value();
  request.accept_gzip = internal_protocol::PrefersGzip(*accept.value());
  return Result<StreamingRequest>(std::move(request));
}
Result<Metadata> EncodeStreamingResponse(const StreamingResponse& response) {
  if (response.protocol != Protocol::kGrpcWeb && response.text)
    return Result<Metadata>(Invalid("text encoding requires gRPC-Web"));
  if (response.protocol == Protocol::kGrpcWeb)
    return EncodeGrpcWebResponse(response);
  if (response.protocol == Protocol::kGrpc) return EncodeGrpcResponse(response);
  if (response.protocol != Protocol::kConnect)
    return Result<Metadata>(
        {Code::kUnimplemented, "protocol not implemented", {}});
  Status valid = internal_protocol::ValidateMetadata(response.headers);
  if (!valid.ok()) return Result<Metadata>(valid);
  Metadata headers{{":status", "200"},
                   {"content-type", ContentType(response.codec)}};
  if (response.gzip) headers.emplace_back("connect-content-encoding", "gzip");
  headers.insert(headers.end(), response.headers.begin(),
                 response.headers.end());
  return Result<Metadata>(std::move(headers));
}
Result<StreamingResponse> DecodeStreamingResponse(const Metadata& headers,
                                                  Codec expected_codec,
                                                  bool accept_gzip) {
  auto status = Single(headers, ":status");
  if (!status.ok() || *status.value() != "200") {
    // Reuse HTTP fallback mapping without interpreting streaming bodies as
    // unary JSON errors. Streaming responses must use HTTP 200.
    HttpMessage response;
    response.headers = headers;
    auto error = DecodeUnaryResponse(std::move(response), expected_codec, 0);
    return Result<StreamingResponse>(
        error.status.ok()
            ? Status{Code::kInternal, "invalid streaming HTTP status", {}}
            : error.status);
  }
  auto type = Single(headers, "content-type");
  auto codec = type.ok() ? ParseStreamingContentType(*type.value())
                         : Result<Codec>(type.status());
  if (!codec.ok() || *codec.value() != expected_codec)
    return Result<StreamingResponse>(
        {Code::kInternal, "unexpected streaming content type", {}});
  auto gzip = Compression(headers);
  if (!gzip.ok() || (*gzip.value() && !accept_gzip))
    return Result<StreamingResponse>(
        {Code::kInternal, "unexpected streaming compression", {}});
  StreamingResponse response;
  response.codec = *codec.value();
  response.gzip = *gzip.value();
  Metadata ignored;
  internal_protocol::ExtractMetadata(headers, response.headers, ignored);
  return Result<StreamingResponse>(std::move(response));
}
Result<std::string> EncodeEndStream(const StreamCompletion& completion,
                                    const StreamLimits& limits) {
  Status valid = internal_protocol::ValidateMetadata(completion.trailers);
  if (valid.ok())
    valid = MetadataSize(completion.trailers, limits.max_metadata_bytes);
  if (!valid.ok()) return Result<std::string>(valid);
  google::protobuf::Struct object;
  auto& fields = *object.mutable_fields();
  if (!completion.status.ok()) {
    auto error = EncodeError(completion.status);
    if (!error.ok()) return error;
    valid = DecodeMessage(*error.value(), Codec::kJson,
                          *fields["error"].mutable_struct_value());
    if (!valid.ok()) return Result<std::string>(valid);
  }
  for (const auto& [name, value] : completion.trailers) {
    (*fields["metadata"].mutable_struct_value()->mutable_fields())[name]
        .mutable_list_value()
        ->add_values()
        ->set_string_value(value);
  }
  auto json = EncodeMessage(object, Codec::kJson);
  if (!json.ok()) return json;
  if (json.value()->size() > limits.max_metadata_bytes)
    return Result<std::string>(TooLarge());
  return EncodeEnvelope({false, true, std::move(*json.value())},
                        Protocol::kConnect);
}
Result<StreamCompletion> DecodeEndStream(std::string_view payload,
                                         const StreamLimits& limits) {
  if (payload.size() > limits.max_metadata_bytes)
    return Result<StreamCompletion>(TooLarge());
  google::protobuf::Value object;
  if (!DecodeMessage(payload, Codec::kJson, object).ok() ||
      object.kind_case() != google::protobuf::Value::kStructValue)
    return Result<StreamCompletion>(Invalid("invalid terminal JSON"));
  StreamCompletion completion;
  const auto& fields = object.struct_value().fields();
  auto error = fields.find("error");
  if (error != fields.end() &&
      error->second.kind_case() != google::protobuf::Value::kNullValue) {
    if (error->second.kind_case() != google::protobuf::Value::kStructValue)
      return Result<StreamCompletion>(Invalid("invalid terminal error"));
    // The official suite requires tolerant reads: absent/null codes map to
    // unknown while retaining message and details. Writers always emit a code.
    auto encoded = EncodeMessage(error->second.struct_value(), Codec::kJson);
    if (!encoded.ok()) return Result<StreamCompletion>(encoded.status());
    auto decoded = DecodeError(*encoded.value());
    if (!decoded.ok()) return Result<StreamCompletion>(decoded.status());
    completion.status = std::move(decoded.value()->status);
  }
  auto metadata = fields.find("metadata");
  if (metadata != fields.end()) {
    if (metadata->second.kind_case() != google::protobuf::Value::kStructValue)
      return Result<StreamCompletion>(Invalid("invalid terminal metadata"));
    for (const auto& [name, values] :
         metadata->second.struct_value().fields()) {
      Status valid_name =
          internal_protocol::ValidateMetadata({{Lower(name), ""}});
      if (!valid_name.ok()) return Result<StreamCompletion>(valid_name);
      if (values.kind_case() != google::protobuf::Value::kListValue)
        return Result<StreamCompletion>(
            Invalid("metadata values must be arrays"));
      for (const auto& value : values.list_value().values()) {
        if (value.kind_case() != google::protobuf::Value::kStringValue)
          return Result<StreamCompletion>(
              Invalid("metadata values must be strings"));
        completion.trailers.emplace_back(Lower(name), value.string_value());
      }
    }
    Status valid = internal_protocol::ValidateMetadata(completion.trailers);
    if (valid.ok())
      valid = MetadataSize(completion.trailers, limits.max_metadata_bytes);
    if (!valid.ok()) return Result<StreamCompletion>(valid);
  }
  return Result<StreamCompletion>(std::move(completion));
}
Result<std::string> EncodeStreamingMessage(std::string payload, bool gzip,
                                           const StreamLimits& limits) {
  if (payload.size() > limits.max_decompressed_bytes)
    return Result<std::string>(TooLarge());
  bool compressed = gzip && !payload.empty();
  if (compressed) {
    auto encoded = Gzip(payload);
    if (!encoded.ok()) return encoded;
    payload = std::move(*encoded.value());
  }
  if (payload.size() > limits.max_message_bytes)
    return Result<std::string>(TooLarge());
  return EncodeEnvelope({compressed, false, std::move(payload)},
                        Protocol::kConnect);
}
StreamingDecoder::StreamingDecoder(bool response, bool gzip,
                                   StreamLimits limits, Protocol protocol)
    : response_(response),
      gzip_(gzip),
      protocol_(protocol),
      limits_(limits),
      decoder_(protocol, limits.max_message_bytes, limits.max_metadata_bytes) {
  if (protocol != Protocol::kConnect && protocol != Protocol::kGrpc &&
      protocol != Protocol::kGrpcWeb)
    error_ = {Code::kUnimplemented, "message protocol not implemented", {}};
}
Result<std::optional<std::string>> StreamingDecoder::ReadOne(
    std::string_view& bytes) {
  using Next = Result<std::optional<std::string>>;
  if (!error_.ok()) return Next(error_);
  auto envelope = decoder_.ReadOne(bytes);
  if (!envelope.ok()) return Next(error_ = envelope.status());
  if (!*envelope.value()) return Next(std::nullopt);
  Envelope& value = **envelope.value();
  if (value.terminal && !response_)
    return Next(error_ = Invalid("terminal envelope in request"));
  if (value.compressed) {
    if (!gzip_)
      return Next(error_ = Status{Code::kInternal,
                                  "compression flag without negotiation",
                                  {}});
    size_t limit = value.terminal ? limits_.max_metadata_bytes
                                  : limits_.max_decompressed_bytes;
    auto decoded = Gunzip(value.payload, limit);
    if (!decoded.ok()) return Next(error_ = decoded.status());
    value.payload = std::move(*decoded.value());
  }
  if (value.terminal) {
    auto completion = protocol_ == Protocol::kGrpcWeb
                          ? DecodeGrpcWebTrailers(value.payload, limits_)
                          : DecodeEndStream(value.payload, limits_);
    if (!completion.ok()) return Next(error_ = completion.status());
    completion_ = std::move(*completion.value());
    return Next(std::nullopt);
  }
  if (value.payload.size() > limits_.max_decompressed_bytes)
    return Next(error_ = TooLarge());
  return Next(std::move(value.payload));
}
Result<StreamCompletion> StreamingDecoder::Finish() const {
  if (!error_.ok()) return Result<StreamCompletion>(error_);
  Status complete = decoder_.Finish();
  if (!complete.ok()) return Result<StreamCompletion>(complete);
  if (response_ && protocol_ != Protocol::kGrpc && !completion_)
    return Result<StreamCompletion>(
        Status{Code::kInternal, "missing terminal envelope", {}});
  return Result<StreamCompletion>(completion_.value_or(StreamCompletion{}));
}
}  // namespace connectrpc
