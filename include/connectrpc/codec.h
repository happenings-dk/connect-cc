#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_CODEC_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_CODEC_H_
#include <string>
#include <string_view>

#include "connectrpc/status.h"
#include "google/protobuf/message_lite.h"
#ifndef CONNECTCPP_PROTOBUF_LITE
#include "google/protobuf/message.h"
#endif
namespace connectrpc {
enum class Codec { kProtobuf, kJson };
// Lite messages support binary protobuf; JSON returns Unimplemented.
Result<std::string> EncodeMessage(const google::protobuf::MessageLite& message,
                                  Codec codec);
Result<std::string> EncodeDeterministicMessage(
    const google::protobuf::MessageLite& message, Codec codec);
Status DecodeMessage(std::string_view bytes, Codec codec,
                     google::protobuf::MessageLite& message);
#ifndef CONNECTCPP_PROTOBUF_LITE
Result<std::string> EncodeMessage(const google::protobuf::Message& message,
                                  Codec codec);
// Stable bytes for the same message, including maps. Used for GET cache keys;
// not a canonical cross-version protobuf representation.
Result<std::string> EncodeDeterministicMessage(
    const google::protobuf::Message& message, Codec codec);
Status DecodeMessage(std::string_view bytes, Codec codec,
                     google::protobuf::Message& message);
#endif
Result<std::string> Gzip(std::string_view bytes);
// Decodes every consecutive gzip member under one aggregate output limit.
// Rejects malformed/truncated members and non-gzip trailing bytes.
Result<std::string> Gunzip(std::string_view bytes, size_t max_output_bytes);
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_CODEC_H_
