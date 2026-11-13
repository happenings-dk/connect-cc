#include "connectrpc/grpc_web_protocol.h"

#include <algorithm>
#include <utility>

#include "absl/strings/escaping.h"
#include "connectrpc/grpc_protocol.h"
#include "protocol_internal.h"

namespace connectrpc {
namespace {
using internal_protocol::Single;
Status Invalid(std::string message) {
  return {Code::kInvalidArgument, std::move(message), {}};
}
std::string ContentType(Codec codec, bool text) {
  return std::string(text ? "application/grpc-web-text+"
                          : "application/grpc-web+") +
         (codec == Codec::kJson ? "json" : "proto");
}
Metadata Translate(const Metadata& headers, Codec codec) {
  Metadata result;
  for (const auto& [name, value] : headers) {
    if (name == "accept" || name == "x-grpc-web" || name == "x-user-agent")
      continue;
    result.emplace_back(name,
                        name == "content-type"
                            ? (codec == Codec::kJson ? "application/grpc+json"
                                                     : "application/grpc+proto")
                            : value);
  }
  return result;
}
void SetType(Metadata& headers, Codec codec, bool text) {
  for (auto& [name, value] : headers)
    if (name == "content-type") value = ContentType(codec, text);
}
bool Token(std::string_view name) {
  if (name.empty()) return false;
  for (unsigned char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos)
      continue;
    return false;
  }
  return true;
}
}  // namespace
Result<GrpcWebContentType> ParseGrpcWebContentType(std::string_view type) {
  std::string normalized =
      internal_protocol::Lower(internal_protocol::Trim(type));
  bool text = normalized.starts_with("application/grpc-web-text");
  std::string_view prefix =
      text ? "application/grpc-web-text" : "application/grpc-web";
  if (!normalized.starts_with(prefix))
    return Result<GrpcWebContentType>(
        Status{Code::kUnimplemented, "unsupported gRPC-Web content type", {}});
  auto codec = ParseGrpcContentType("application/grpc" +
                                    normalized.substr(prefix.size()));
  if (!codec.ok()) return Result<GrpcWebContentType>(codec.status());
  return Result<GrpcWebContentType>(GrpcWebContentType{*codec.value(), text});
}
Result<Metadata> EncodeGrpcWebRequest(const StreamingRequest& request) {
  for (const auto& [name, value] : request.headers)
    if (name == "accept")
      return Result<Metadata>(Invalid("gRPC-Web owns the accept header"));
  auto headers = EncodeGrpcRequest(request);
  if (!headers.ok()) return headers;
  SetType(*headers.value(), request.codec, request.text);
  std::erase_if(*headers.value(),
                [](const auto& field) { return field.first == "te"; });
  headers.value()->emplace_back(
      "accept", ContentType(request.codec, request.accept_text));
  return headers;
}
Result<StreamingRequest> DecodeGrpcWebRequest(const Metadata& headers) {
  auto type = Single(headers, "content-type");
  if (!type.ok()) return Result<StreamingRequest>(type.status());
  auto parsed = ParseGrpcWebContentType(*type.value());
  if (!parsed.ok()) return Result<StreamingRequest>(parsed.status());
  auto result = DecodeGrpcRequest(Translate(headers, parsed.value()->codec));
  if (!result.ok()) return result;
  auto accept = Single(headers, "accept");
  if (!accept.ok()) return Result<StreamingRequest>(accept.status());
  result.value()->protocol = Protocol::kGrpcWeb;
  result.value()->text = parsed.value()->text;
  result.value()->accept_text = parsed.value()->text;
  if (!accept.value()->empty()) {
    auto response_type = ParseGrpcWebContentType(*accept.value());
    if (response_type.ok())
      result.value()->accept_text = response_type.value()->text;
  }
  return result;
}
Result<Metadata> EncodeGrpcWebResponse(const StreamingResponse& response) {
  auto headers = EncodeGrpcResponse(response);
  if (headers.ok()) SetType(*headers.value(), response.codec, response.text);
  return headers;
}
Result<StreamingResponse> DecodeGrpcWebResponse(const Metadata& headers,
                                                Codec expected_codec,
                                                bool accept_gzip) {
  auto type = Single(headers, "content-type");
  auto parsed = type.ok() ? ParseGrpcWebContentType(*type.value())
                          : Result<GrpcWebContentType>(type.status());
  // Preserve the gRPC HTTP fallback mapping before validating media type.
  auto status = Single(headers, ":status");
  if (!status.ok() || *status.value() != "200")
    return DecodeGrpcResponse(headers, expected_codec, accept_gzip);
  if (!parsed.ok())
    return Result<StreamingResponse>(
        Status{Code::kUnknown, "unexpected gRPC-Web content type", {}});
  auto result = DecodeGrpcResponse(Translate(headers, parsed.value()->codec),
                                   expected_codec, accept_gzip);
  if (result.ok()) {
    result.value()->protocol = Protocol::kGrpcWeb;
    result.value()->text = parsed.value()->text;
  }
  return result;
}
Result<std::string> EncodeGrpcWebTrailers(const StreamCompletion& completion,
                                          const StreamLimits& limits) {
  auto trailers = EncodeGrpcTrailers(completion, limits);
  if (!trailers.ok()) return Result<std::string>(trailers.status());
  std::string payload;
  for (const auto& [name, value] : *trailers.value())
    payload += name + ": " + value + "\r\n";
  if (payload.size() > limits.max_metadata_bytes)
    return Result<std::string>(
        Status{Code::kResourceExhausted, "gRPC-Web trailers exceed limit", {}});
  return EncodeEnvelope({false, true, std::move(payload)}, Protocol::kGrpcWeb);
}
Result<StreamCompletion> DecodeGrpcWebTrailers(std::string_view payload,
                                               const StreamLimits& limits) {
  if (payload.size() > limits.max_metadata_bytes)
    return Result<StreamCompletion>(
        Status{Code::kResourceExhausted, "gRPC-Web trailers exceed limit", {}});
  Metadata trailers;
  while (!payload.empty()) {
    size_t end = payload.find("\r\n");
    // Tolerate a missing final CRLF on reads; writers always include it.
    auto line = payload.substr(0, end);
    size_t colon = line.find(':');
    std::string name = internal_protocol::Lower(line.substr(0, colon));
    if (colon == std::string_view::npos || !Token(name))
      return Result<StreamCompletion>(Invalid("invalid gRPC-Web trailer name"));
    auto value = internal_protocol::Trim(line.substr(colon + 1));
    for (unsigned char c : value)
      if ((c < 0x20 && c != '\t') || c == 0x7f)
        return Result<StreamCompletion>(
            Invalid("invalid gRPC-Web trailer value"));
    trailers.emplace_back(std::move(name), value);
    if (end == std::string_view::npos) break;
    payload.remove_prefix(end + 2);
  }
  return DecodeGrpcTrailers(trailers, limits);
}
Result<std::string> GrpcWebTextDecoder::Read(std::string_view bytes) {
  if (!error_.ok()) return Result<std::string>(error_);
  std::string output;
  for (char c : bytes) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='))
      return Result<std::string>(error_ = Invalid("invalid gRPC-Web base64"));
    quartet_.push_back(c);
    if (quartet_.size() != 4) continue;
    std::string decoded;
    if (quartet_[0] == '=' || quartet_[1] == '=' ||
        (quartet_[2] == '=' && quartet_[3] != '=') ||
        !absl::Base64Unescape(quartet_, &decoded))
      return Result<std::string>(
          error_ = Invalid("invalid gRPC-Web base64 padding"));
    output += decoded;
    quartet_.clear();
  }
  return Result<std::string>(std::move(output));
}
Status GrpcWebTextDecoder::Finish() const {
  if (!error_.ok()) return error_;
  if (!quartet_.empty()) return Invalid("truncated gRPC-Web base64");
  return {};
}
}  // namespace connectrpc
