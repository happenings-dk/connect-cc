#ifndef CONNECTCPP_SRC_PROTOCOL_INTERNAL_H_
#define CONNECTCPP_SRC_PROTOCOL_INTERNAL_H_

#include <string>
#include <string_view>

#include "connectrpc/http2.h"
#include "connectrpc/status.h"

namespace connectrpc::internal_protocol {
std::string_view Trim(std::string_view text);
std::string Lower(std::string_view text);
Result<std::string> Single(const Metadata& headers, std::string_view key);
bool Reserved(std::string_view name);
Status ValidateMetadata(const Metadata& metadata);
bool AcceptsGzip(std::string_view value);
// Connect treats the list as ordered preferences; implicit identity is last.
bool PrefersGzip(std::string_view value);
void ExtractMetadata(const Metadata& wire, Metadata& headers,
                     Metadata& trailers);
}  // namespace connectrpc::internal_protocol
#endif  // CONNECTCPP_SRC_PROTOCOL_INTERNAL_H_
