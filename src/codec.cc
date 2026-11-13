#include "connectrpc/codec.h"

#include <zlib.h>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/struct.pb.h"
#include "google/protobuf/util/json_util.h"
namespace connectrpc {
Result<std::string> EncodeMessage(const google::protobuf::Message& message,
                                  Codec codec) {
  std::string bytes;
  bool ok =
      codec == Codec::kProtobuf
          ? message.SerializeToString(&bytes)
          : google::protobuf::util::MessageToJsonString(message, &bytes).ok();
  if (!ok) return Result<std::string>({Code::kInternal, "encode failed", {}});
  return Result<std::string>(std::move(bytes));
}
namespace {
Status StableJson(const google::protobuf::Value& value, int depth,
                  std::string& output) {
  if (depth > 100)
    return {Code::kResourceExhausted, "JSON nesting exceeds limit", {}};
  if (value.kind_case() == google::protobuf::Value::kStructValue) {
    const auto& fields = value.struct_value().fields();
    std::vector<std::string> names;
    names.reserve(fields.size());
    for (const auto& [name, field] : fields) {
      (void)field;
      names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    output.push_back('{');
    bool first = true;
    for (const auto& name : names) {
      if (!first) output.push_back(',');
      first = false;
      google::protobuf::Value key;
      key.set_string_value(name);
      Status status = StableJson(key, depth + 1, output);
      if (!status.ok()) return status;
      output.push_back(':');
      status = StableJson(fields.at(name), depth + 1, output);
      if (!status.ok()) return status;
    }
    output.push_back('}');
  } else if (value.kind_case() == google::protobuf::Value::kListValue) {
    output.push_back('[');
    bool first = true;
    for (const auto& item : value.list_value().values()) {
      if (!first) output.push_back(',');
      first = false;
      Status status = StableJson(item, depth + 1, output);
      if (!status.ok()) return status;
    }
    output.push_back(']');
  } else {
    std::string scalar;
    if (!google::protobuf::util::MessageToJsonString(value, &scalar).ok())
      return {Code::kInternal, "encode JSON scalar failed", {}};
    output.append(scalar);
  }
  return {};
}
}  // namespace
Result<std::string> EncodeDeterministicMessage(
    const google::protobuf::Message& message, Codec codec) {
  std::string bytes;
  if (codec == Codec::kProtobuf) {
    bool ok;
    {
      google::protobuf::io::StringOutputStream sink(&bytes);
      google::protobuf::io::CodedOutputStream stream(&sink);
      stream.SetSerializationDeterministic(true);
      ok = message.SerializeToCodedStream(&stream) && !stream.HadError();
    }
    if (!ok) return Result<std::string>({Code::kInternal, "encode failed", {}});
  } else {
    auto json = EncodeMessage(message, codec);
    if (!json.ok()) return json;
    google::protobuf::Value value;
    if (!google::protobuf::util::JsonStringToMessage(*json.value(), &value)
             .ok())
      return Result<std::string>(
          {Code::kInternal, "parse generated JSON failed", {}});
    Status status = StableJson(value, 0, bytes);
    if (!status.ok()) return Result<std::string>(status);
  }
  return Result<std::string>(std::move(bytes));
}
Status DecodeMessage(std::string_view bytes, Codec codec,
                     google::protobuf::Message& message) {
  if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return {Code::kResourceExhausted, "message too large", {}};
  }
  bool ok =
      codec == Codec::kProtobuf
          ? message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))
          : google::protobuf::util::JsonStringToMessage(bytes, &message).ok();
  return ok ? Status{} : Status{Code::kInvalidArgument, "decode failed", {}};
}
namespace {
Result<std::string> Transform(std::string_view bytes, size_t limit,
                              bool compress) {
  if (bytes.size() > std::numeric_limits<uInt>::max()) {
    return Result<std::string>(
        {Code::kResourceExhausted, "input too large", {}});
  }
  z_stream stream{};
  int code = compress ? deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                     31, 8, Z_DEFAULT_STRATEGY)
                      : inflateInit2(&stream, 31);
  if (code != Z_OK)
    return Result<std::string>({Code::kInternal, "gzip init failed", {}});
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
  stream.avail_in = static_cast<uInt>(bytes.size());
  std::string output;
  Status status;
  do {
    char chunk[16384];
    stream.next_out = reinterpret_cast<Bytef*>(chunk);
    stream.avail_out = sizeof(chunk);
    code = compress ? deflate(&stream, Z_FINISH) : inflate(&stream, Z_NO_FLUSH);
    size_t count = sizeof(chunk) - stream.avail_out;
    if (count > limit - output.size()) {
      status = {Code::kResourceExhausted, "gzip output exceeds limit", {}};
      break;
    }
    output.append(chunk, count);
    // RFC 1952 permits consecutive members in one gzip representation. Reset
    // only the inflater, retaining unread input and the aggregate output bound.
    if (!compress && code == Z_STREAM_END && stream.avail_in != 0) {
      code = inflateReset(&stream);
      if (code != Z_OK) {
        status = {Code::kInternal, "gzip reset failed", {}};
        break;
      }
    }
  } while (code == Z_OK);
  if (status.ok() && (code != Z_STREAM_END || stream.avail_in != 0)) {
    status = {Code::kInvalidArgument, "invalid or trailing gzip data", {}};
  }
  if (compress)
    deflateEnd(&stream);
  else
    inflateEnd(&stream);
  if (!status.ok()) return Result<std::string>(std::move(status));
  return Result<std::string>(std::move(output));
}
}  // namespace
Result<std::string> Gzip(std::string_view bytes) {
  return Transform(bytes, std::numeric_limits<size_t>::max(), true);
}
Result<std::string> Gunzip(std::string_view bytes, size_t max_output_bytes) {
  return Transform(bytes, max_output_bytes, false);
}
}  // namespace connectrpc
