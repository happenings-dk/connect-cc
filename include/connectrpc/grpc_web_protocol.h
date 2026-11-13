#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_GRPC_WEB_PROTOCOL_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_GRPC_WEB_PROTOCOL_H_

#include <string>
#include <string_view>

#include "connectrpc/streaming_protocol.h"

namespace connectrpc {
struct GrpcWebContentType {
  Codec codec = Codec::kProtobuf;
  bool text = false;
};
Result<GrpcWebContentType> ParseGrpcWebContentType(std::string_view type);
Result<Metadata> EncodeGrpcWebRequest(const StreamingRequest& request);
Result<StreamingRequest> DecodeGrpcWebRequest(const Metadata& headers);
Result<Metadata> EncodeGrpcWebResponse(const StreamingResponse& response);
Result<StreamingResponse> DecodeGrpcWebResponse(const Metadata& headers,
                                                Codec expected_codec,
                                                bool accept_gzip);
Result<std::string> EncodeGrpcWebTrailers(const StreamCompletion& completion,
                                          const StreamLimits& limits = {});
Result<StreamCompletion> DecodeGrpcWebTrailers(std::string_view payload,
                                               const StreamLimits& limits = {});
// Streaming base64 permits separately padded segments anywhere in the body,
// including inside an envelope. Holds at most three bytes between calls.
// Output is bounded by the supplied chunk size plus two bytes.
class GrpcWebTextDecoder {
 public:
  Result<std::string> Read(std::string_view bytes);
  Status Finish() const;

 private:
  std::string quartet_;
  Status error_;
};
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_GRPC_WEB_PROTOCOL_H_
