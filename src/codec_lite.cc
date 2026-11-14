#include <limits>

#include "connectrpc/codec.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"

namespace connectrpc {
namespace {
Status NoJson() {
  return {
      Code::kUnimplemented, "Protobuf Lite messages do not support JSON", {}};
}
}  // namespace
Result<std::string> EncodeMessage(const google::protobuf::MessageLite& message,
                                  Codec codec) {
  if (codec != Codec::kProtobuf) return Result<std::string>(NoJson());
  std::string bytes;
  if (!message.SerializeToString(&bytes))
    return Result<std::string>({Code::kInternal, "encode failed", {}});
  return Result<std::string>(std::move(bytes));
}
Result<std::string> EncodeDeterministicMessage(
    const google::protobuf::MessageLite& message, Codec codec) {
  if (codec != Codec::kProtobuf) return Result<std::string>(NoJson());
  std::string bytes;
  bool ok;
  {
    google::protobuf::io::StringOutputStream sink(&bytes);
    google::protobuf::io::CodedOutputStream stream(&sink);
    stream.SetSerializationDeterministic(true);
    ok = message.SerializeToCodedStream(&stream) && !stream.HadError();
  }
  if (!ok) return Result<std::string>({Code::kInternal, "encode failed", {}});
  return Result<std::string>(std::move(bytes));
}
Status DecodeMessage(std::string_view bytes, Codec codec,
                     google::protobuf::MessageLite& message) {
  if (codec != Codec::kProtobuf) return NoJson();
  if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    return {Code::kResourceExhausted, "message too large", {}};
  return message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))
             ? Status{}
             : Status{Code::kInvalidArgument, "decode failed", {}};
}
}  // namespace connectrpc
