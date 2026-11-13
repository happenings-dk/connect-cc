#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_HTTP1_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_HTTP1_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "connectrpc/http2.h"
#include "connectrpc/status.h"

namespace connectrpc {
enum class Http1Role { kClient, kServer };
struct Http1Limits {
  size_t max_metadata_bytes = 16 * 1024;
  size_t max_chunk_bytes = 16 * 1024;
};
struct Http1Event {
  enum class Kind { kHeaders, kBody, kEnd };
  Kind kind = Kind::kHeaders;
  // Headers use the same lowercase/pseudo-header representation as HTTP/2.
  // An end event carries HTTP trailers. A body event carries only body bytes.
  Metadata headers;
  std::string body;
  bool keep_alive = false;
};
// Strict incremental HTTP/1.1 framing. No body aggregation. ReadOne advances
// the input through at most one event and at most max_chunk_bytes. An empty
// input can still produce an end event after the last body chunk. Keep calling
// until no event is produced before requesting more network data.
// Calls must be serialized. The class is not movable: llhttp retains callbacks
// into its state. Parsing errors are sticky; a failed parser cannot be reused.
class Http1Parser {
 public:
  static Result<std::unique_ptr<Http1Parser>> Create(Http1Role role,
                                                     Http1Limits limits = {});
  ~Http1Parser();
  Http1Parser(const Http1Parser&) = delete;
  Http1Parser& operator=(const Http1Parser&) = delete;
  Result<std::optional<Http1Event>> ReadOne(std::string_view& bytes);
  // Signals transport EOF. Completes close-delimited responses, or reports a
  // truncated message. Call after draining ReadOne's events (including with
  // empty input). Nullopt means the previous message was already complete.
  Result<std::optional<Http1Event>> EndInput();
  // Explicitly admit a subsequent message after kEnd on a persistent
  // connection. Input belonging to it was left unconsumed by ReadOne.
  Status NextMessage();

 private:
  class Impl;
  explicit Http1Parser(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
// Stateful HTTP/1.1 writer. Uses explicit content-length when supplied,
// otherwise chunked transfer. GET and OPTIONS requests default to zero length.
// Body input must be split into max_chunk_bytes by the caller. End verifies
// content length and emits the final chunk/trailers. The writer handles one
// message; construct a new one for each request/response on a connection.
class Http1Encoder {
 public:
  explicit Http1Encoder(Http1Role role, Http1Limits limits = {});
  Result<std::string> Headers(const Metadata& headers, bool keep_alive = true);
  Result<std::string> Body(std::string_view bytes);
  Result<std::string> End(const Metadata& trailers = {});

 private:
  Http1Role role_;
  Http1Limits limits_;
  std::optional<uint64_t> remaining_;
  bool started_ = false;
  bool ended_ = false;
  bool chunked_ = false;
};
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_HTTP1_H_
