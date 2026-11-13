#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "absl/strings/escaping.h"
#include "get_internal.h"
#include "protocol_internal.h"

namespace connectrpc::internal_get {
namespace {
Status Invalid(std::string message) {
  return {Code::kInvalidArgument, std::move(message), {}};
}
Status TooLarge() {
  return {Code::kResourceExhausted, "GET query exceeds limit", {}};
}
int Hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
Result<std::string> Unescape(std::string_view text, size_t limit) {
  std::string output;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '%') {
      if (text.size() - i < 3 || Hex(text[i + 1]) < 0 || Hex(text[i + 2]) < 0)
        return Result<std::string>(Invalid("invalid GET percent encoding"));
      c = static_cast<char>((Hex(text[i + 1]) << 4) | Hex(text[i + 2]));
      i += 2;
    } else if (c == '+') {
      c = ' ';
    }
    if (output.size() == limit) return Result<std::string>(TooLarge());
    output.push_back(c);
  }
  return Result<std::string>(std::move(output));
}
bool Utf8(std::string_view value) {
  for (size_t i = 0; i < value.size();) {
    unsigned char first = value[i++];
    if (first < 0x80) continue;
    int count;
    uint32_t code;
    uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 1;
      code = first & 0x1f;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 2;
      code = first & 0x0f;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 3;
      code = first & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (value.size() - i < static_cast<size_t>(count)) return false;
    for (int n = 0; n < count; ++n) {
      unsigned char next = value[i++];
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
      return false;
  }
  return true;
}
bool Base64(std::string_view text) {
  size_t padding = 0;
  for (char c : text) {
    if (c == '=') {
      if (++padding > 2) return false;
      continue;
    }
    if (padding || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  }
  return true;
}
Status EscapeAppend(std::string_view text, size_t limit, std::string& output) {
  constexpr char kHex[] = "0123456789ABCDEF";
  for (unsigned char c : text) {
    bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                c == '~';
    size_t count = safe ? 1 : 3;
    if (output.size() > limit || count > limit - output.size())
      return TooLarge();
    if (safe)
      output.push_back(static_cast<char>(c));
    else {
      output.push_back('%');
      output.push_back(kHex[c >> 4]);
      output.push_back(kHex[c & 15]);
    }
  }
  return {};
}
bool Owned(std::string_view name) {
  return name == "content-type" || name == "content-encoding" ||
         name == "connect-protocol-version" || name == "content-length";
}
}  // namespace
Status Encode(HttpMessage& request, bool compressed, size_t max_url_bytes) {
  std::string path;
  std::string codec;
  for (const auto& [name, value] : request.headers) {
    if (name == ":path") path = value;
    if (name == "content-type")
      codec = value == "application/json" ? "json" : "proto";
  }
  if (path.size() > max_url_bytes || request.body.size() > max_url_bytes)
    return TooLarge();
  bool base64 = compressed || codec == "proto";
  path += "?connect=v1";
  if (base64) path += "&base64=1";
  if (compressed) path += "&compression=gzip";
  path += "&encoding=" + codec + "&message=";
  if (path.size() > max_url_bytes) return TooLarge();
  std::string payload = base64 ? absl::WebSafeBase64Escape(request.body)
                               : std::move(request.body);
  if (base64)
    while (!payload.empty() && payload.back() == '=') payload.pop_back();
  Status status = EscapeAppend(payload, max_url_bytes, path);
  if (!status.ok()) return status;
  std::erase_if(request.headers,
                [](const auto& field) { return Owned(field.first); });
  for (auto& [name, value] : request.headers) {
    if (name == ":method") value = "GET";
    if (name == ":path") value = path;
  }
  request.body.clear();
  return {};
}
Decoded Decode(HttpMessage request, size_t max_bytes) {
  Decoded result;
  if (!request.body.empty()) {
    result.status = Invalid("GET request must not have a body");
    return result;
  }
  auto target = internal_protocol::Single(request.headers, ":path");
  if (!target.ok()) {
    result.status = target.status();
    return result;
  }
  size_t question = target.value()->find('?');
  if (question == std::string::npos ||
      target.value()->find('#') != std::string::npos) {
    result.status = Invalid("missing or invalid GET query");
    return result;
  }
  std::string procedure = target.value()->substr(0, question);
  std::map<std::string, std::string, std::less<>> query;
  std::string_view rest(*target.value());
  rest.remove_prefix(question + 1);
  size_t encoded_limit = max_bytes < std::numeric_limits<size_t>::max() / 2
                             ? max_bytes + max_bytes / 3 + 4
                             : std::numeric_limits<size_t>::max();
  while (!rest.empty()) {
    size_t amp = rest.find('&');
    std::string_view pair = rest.substr(0, amp);
    size_t equal = pair.find('=');
    // No recognized key exceeds "compression". Longer keys are unknown and
    // must be ignored without allocating memory proportional to their size.
    auto name = Unescape(pair.substr(0, equal), 11);
    if (!name.ok() && name.status().code != Code::kResourceExhausted) {
      result.status = name.status();
      return result;
    }
    if (name.ok() &&
        (*name.value() == "message" || *name.value() == "encoding" ||
         *name.value() == "compression" || *name.value() == "base64" ||
         *name.value() == "connect")) {
      auto value =
          Unescape(equal == std::string_view::npos ? std::string_view{}
                                                   : pair.substr(equal + 1),
                   *name.value() == "message" ? encoded_limit : 128);
      if (!value.ok()) {
        result.status = value.status();
        return result;
      }
      if (!query.emplace(*name.value(), std::move(*value.value())).second) {
        result.status = Invalid("duplicate GET query parameter");
        return result;
      }
    }
    if (amp == std::string_view::npos) break;
    rest.remove_prefix(amp + 1);
  }
  if (!query.contains("message") || !query.contains("encoding")) {
    result.status = Invalid("missing GET message or encoding");
    return result;
  }
  if (query.contains("connect") && query.at("connect") != "v1") {
    result.status = Invalid("unsupported GET protocol version");
    return result;
  }
  const auto& codec = query.at("encoding");
  if (codec != "proto" && codec != "json") {
    result.status = {Code::kUnimplemented, "unsupported GET encoding", {}};
    result.http_status = 415;
    return result;
  }
  std::string payload = std::move(query.at("message"));
  if (query.contains("base64") && query.at("base64") == "1") {
    std::string bytes;
    if (!Base64(payload) || !absl::WebSafeBase64Unescape(payload, &bytes)) {
      result.status = Invalid("invalid GET base64");
      return result;
    }
    payload = std::move(bytes);
  } else if (!Utf8(payload)) {
    result.status = Invalid("GET text must be UTF-8");
    return result;
  }
  if (payload.size() > max_bytes) {
    result.status = TooLarge();
    return result;
  }
  std::erase_if(request.headers,
                [](const auto& field) { return Owned(field.first); });
  for (auto& [name, value] : request.headers) {
    if (name == ":method") value = "POST";
    if (name == ":path") value = procedure;
  }
  request.headers.emplace_back("content-type", "application/" + codec);
  if (query.contains("compression"))
    request.headers.emplace_back("content-encoding", query.at("compression"));
  request.body = std::move(payload);
  result.message = std::move(request);
  return result;
}
}  // namespace connectrpc::internal_get
