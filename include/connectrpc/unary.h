#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_UNARY_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_UNARY_H_

#include <chrono>
#include <string>
#include <string_view>

#include "connectrpc/codec.h"
#include "connectrpc/envelope.h"
#include "connectrpc/http2.h"
#include "connectrpc/transport.h"

namespace connectrpc {
enum class Idempotency { kUnknown, kIdempotent, kNoSideEffects };
// Schema-derived method semantics. Only kNoSideEffects permits Connect GET.
struct MethodOptions {
  Idempotency idempotency = Idempotency::kUnknown;
};
// Metadata is HTTP-ready: binary values use -bin names and base64 values.
// Protocol-owned headers cannot be overridden through call metadata.
struct CallOptions {
  Codec codec = Codec::kProtobuf;
  Metadata headers;
  bool compress = false;
  TransportOptions transport;
  Protocol protocol = Protocol::kConnect;
  // Base64 request body and text response negotiation; requires kGrpcWeb.
  bool grpc_web_text = false;
  // Opt-in GET for Connect unary methods declared kNoSideEffects. No fallback
  // to POST: overlong targets fail before sending an RPC.
  bool use_get = false;
};
struct UnaryRequest {
  std::string procedure;
  Codec codec = Codec::kProtobuf;
  std::string payload;
  Metadata headers;
  bool accept_gzip = false;
  bool is_get = false;
  std::chrono::steady_clock::time_point deadline =
      (std::chrono::steady_clock::time_point::max)();
};
// Preserves leading and trailing metadata even when status is non-OK.
struct UnaryResponse {
  Status status;
  std::string payload;
  Metadata headers;
  Metadata trailers;
};
std::string_view CodeName(Code code);
int HttpStatus(Code code);
Code CodeFromHttpStatus(int status);
Result<Codec> ParseUnaryContentType(std::string_view value);
Result<std::string> EncodeError(const Status& status);
struct DecodedError {
  Status status;
};
Result<DecodedError> DecodeError(std::string_view json,
                                 Code fallback_code = Code::kUnknown);
Result<HttpMessage> EncodeUnaryRequest(std::string procedure,
                                       std::string payload,
                                       const CallOptions& options,
                                       MethodOptions method = {});
Result<UnaryRequest> DecodeUnaryRequest(HttpMessage request, size_t max_bytes);
Result<HttpMessage> EncodeUnaryResponse(UnaryResponse response, Codec codec,
                                        bool compress);
UnaryResponse DecodeUnaryResponse(HttpMessage response, Codec codec,
                                  size_t max_bytes);
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_UNARY_H_
