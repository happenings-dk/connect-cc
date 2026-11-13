#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_STREAMING_PROTOCOL_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_STREAMING_PROTOCOL_H_

#include <stddef.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "connectrpc/codec.h"
#include "connectrpc/envelope.h"
#include "connectrpc/http2.h"
#include "connectrpc/status.h"

namespace connectrpc {
struct StreamLimits {
  size_t max_message_bytes = 4 * 1024 * 1024;
  size_t max_decompressed_bytes = 4 * 1024 * 1024;
  size_t max_metadata_bytes = 16 * 1024;
};
enum class StreamShape {
  kServerStreaming,
  kClientStreaming,
  kBidiStreaming,
  kUnary
};
struct StreamingRequest {
  std::string procedure;
  Codec codec = Codec::kProtobuf;
  Metadata headers;
  bool gzip = false;
  bool accept_gzip = true;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  // Local schema information; not transmitted in HTTP headers.
  StreamShape shape = StreamShape::kBidiStreaming;
  Protocol protocol = Protocol::kConnect;
  bool text = false;
  bool accept_text = false;
};
struct StreamingResponse {
  Codec codec = Codec::kProtobuf;
  Metadata headers;
  bool gzip = false;
  Protocol protocol = Protocol::kConnect;
  bool text = false;
};
struct StreamCompletion {
  Status status;
  Metadata trailers;
};
Result<Codec> ParseStreamingContentType(std::string_view type);
Result<Metadata> EncodeStreamingRequest(const StreamingRequest& request);
Result<StreamingRequest> DecodeStreamingRequest(const Metadata& headers);
Result<Metadata> EncodeStreamingResponse(const StreamingResponse& response);
Result<StreamingResponse> DecodeStreamingResponse(const Metadata& headers,
                                                  Codec expected_codec,
                                                  bool accept_gzip);
// Emits an uncompressed JSON terminal envelope, independent of message codec.
Result<std::string> EncodeEndStream(const StreamCompletion& completion,
                                    const StreamLimits& limits = {});
Result<StreamCompletion> DecodeEndStream(std::string_view payload,
                                         const StreamLimits& limits = {});
Result<std::string> EncodeStreamingMessage(std::string payload, bool gzip,
                                           const StreamLimits& limits = {});
// A pull parser for one Connect or gRPC message direction. Holds at most one
// incomplete envelope; callers retain any unconsumed chunk. Connect Finish
// returns the terminal status; gRPC callers separately decode HTTP trailers.
// All access must be serialized by the caller. gRPC-Web uses body trailers.
class StreamingDecoder {
 public:
  StreamingDecoder(bool response, bool gzip, StreamLimits limits = {},
                   Protocol protocol = Protocol::kConnect);
  StreamingDecoder(const StreamingDecoder&) = delete;
  StreamingDecoder& operator=(const StreamingDecoder&) = delete;
  // Nullopt means no message was produced. It may mean partial input or a
  // terminal record; only Finish at HTTP EOF establishes RPC completion.
  Result<std::optional<std::string>> ReadOne(std::string_view& bytes);
  Result<StreamCompletion> Finish() const;

 private:
  bool response_;
  bool gzip_;
  Protocol protocol_;
  StreamLimits limits_;
  EnvelopeDecoder decoder_;
  std::optional<StreamCompletion> completion_;
  Status error_;
};
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_STREAMING_PROTOCOL_H_
