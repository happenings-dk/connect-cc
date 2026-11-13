#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_HTTP2_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_HTTP2_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "connectrpc/status.h"

namespace connectrpc {
// Header names are lowercase. Repeated values retain their order.
using Metadata = std::vector<std::pair<std::string, std::string>>;
struct HttpMessage {
  int32_t stream_id = 0;
  Metadata headers;
  std::string body;
};
// Bounded HTTP/2 unary session engine. Calls must be serialized by the owner.
// The transport feeds received bytes and drains pending output after each call.
// The handler is invoked synchronously when a peer finishes a message.
class Http2Session {
 public:
  enum class Role { kClient, kServer };
  static Result<std::unique_ptr<Http2Session>> Create(
      Role role, size_t max_body_bytes, size_t max_metadata_bytes,
      std::function<void(HttpMessage)> handler,
      std::function<void(int32_t, Status)> on_error = {});
  ~Http2Session();
  Http2Session(const Http2Session&) = delete;
  Http2Session& operator=(const Http2Session&) = delete;
  Result<int32_t> Request(Metadata headers, std::string body);
  Status Respond(int32_t stream_id, Metadata headers, std::string body);
  Status Receive(std::string_view bytes);
  // Defaults to HTTP/2 CANCEL (0x8); protocol engines may specify another code.
  Status Reset(int32_t stream_id, uint32_t error_code = 8);
  Status Shutdown();
  Result<std::string> Drain();

 private:
  struct Impl;
  explicit Http2Session(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_HTTP2_H_
