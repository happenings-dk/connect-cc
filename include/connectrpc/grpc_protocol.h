#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_GRPC_PROTOCOL_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_GRPC_PROTOCOL_H_

#include <chrono>
#include <string>
#include <string_view>

#include "connectrpc/streaming_protocol.h"

namespace connectrpc {
// gRPC's timeout unit is case-sensitive. Writers round upward, choosing the
// finest unit whose value fits eight digits. Readers saturate large deadlines.
Result<std::string> EncodeGrpcTimeout(
    std::chrono::steady_clock::time_point deadline);
Result<std::chrono::steady_clock::time_point> DecodeGrpcTimeout(
    std::string_view timeout);
Result<Codec> ParseGrpcContentType(std::string_view type);
Result<Metadata> EncodeGrpcRequest(const StreamingRequest& request);
Result<StreamingRequest> DecodeGrpcRequest(const Metadata& headers);
Result<Metadata> EncodeGrpcResponse(const StreamingResponse& response);
Result<StreamingResponse> DecodeGrpcResponse(const Metadata& headers,
                                             Codec expected_codec,
                                             bool accept_gzip);
// Status lives in HTTP trailers (or the initial block for trailers-only
// responses). Error messages use percent encoding; details use the binary
// google.rpc.Status schema. Limits include HTTP/2's per-field overhead.
Result<Metadata> EncodeGrpcTrailers(const StreamCompletion& completion,
                                    const StreamLimits& limits = {});
Result<StreamCompletion> DecodeGrpcTrailers(const Metadata& trailers,
                                            const StreamLimits& limits = {});
Code GrpcCodeFromHttpStatus(int status);
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_GRPC_PROTOCOL_H_
