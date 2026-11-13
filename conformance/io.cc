#include <cstdint>
#include <iostream>
#include <map>
#include <string>

#include "conformance/adapter.h"

namespace connectrpc::conformance_adapter {
Result<bool> ReadFrame(google::protobuf::Message& message) {
  unsigned char prefix[4];
  std::cin.read(reinterpret_cast<char*>(prefix), 4);
  if (std::cin.eof() && std::cin.gcount() == 0) return Result<bool>(false);
  if (!std::cin)
    return Result<bool>({Code::kInvalidArgument, "truncated frame prefix", {}});
  uint32_t length = (uint32_t{prefix[0]} << 24) | (uint32_t{prefix[1]} << 16) |
                    (uint32_t{prefix[2]} << 8) | prefix[3];
  if (length > 64 * 1024 * 1024)
    return Result<bool>(
        {Code::kResourceExhausted, "adapter frame exceeds limit", {}});
  std::string bytes(length, '\0');
  std::cin.read(bytes.data(), length);
  if (!std::cin || !message.ParseFromString(bytes))
    return Result<bool>({Code::kInvalidArgument, "invalid adapter frame", {}});
  return Result<bool>(true);
}
Status WriteFrame(const google::protobuf::Message& message) {
  std::string bytes;
  if (!message.SerializeToString(&bytes) || bytes.size() > 64 * 1024 * 1024)
    return {Code::kInternal, "cannot encode adapter result", {}};
  uint32_t length = static_cast<uint32_t>(bytes.size());
  char prefix[4] = {static_cast<char>(length >> 24),
                    static_cast<char>(length >> 16),
                    static_cast<char>(length >> 8), static_cast<char>(length)};
  std::cout.write(prefix, 4).write(bytes.data(), bytes.size()).flush();
  return std::cout ? Status{}
                   : Status{Code::kUnavailable, "adapter output closed", {}};
}
Metadata ReadHeaders(
    const google::protobuf::RepeatedPtrField<conformance::v1::Header>&
        headers) {
  Metadata result;
  for (const auto& header : headers) {
    std::string name = header.name();
    for (char& c : name)
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    for (const auto& value : header.value()) result.emplace_back(name, value);
  }
  return result;
}
void WriteHeaders(
    const Metadata& metadata,
    google::protobuf::RepeatedPtrField<conformance::v1::Header>& headers) {
  std::map<std::string, conformance::v1::Header*> entries;
  for (const auto& [name, value] : metadata) {
    auto [it, inserted] = entries.try_emplace(name, nullptr);
    if (inserted) {
      it->second = headers.Add();
      it->second->set_name(name);
    }
    it->second->add_value(value);
  }
}
}  // namespace connectrpc::conformance_adapter
