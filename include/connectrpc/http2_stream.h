#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_HTTP2_STREAM_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_HTTP2_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "connectrpc/http2.h"
#include "connectrpc/status.h"

namespace connectrpc {
struct Http2StreamOptions {
  size_t max_metadata_bytes = 16 * 1024;
  size_t max_queued_bytes = 64 * 1024;  // Per stream, outbound only.
  uint32_t max_streams = 100;           // Includes closed, unreleased streams.
  // At least 65535: peers may send this much before receiving our SETTINGS.
  int32_t stream_window_bytes = 65535;
  int32_t connection_window_bytes = 1024 * 1024;
};
struct Http2StreamEvents {
  // Headers, zero or more data callbacks, end, then close on normal completion.
  // Informational headers do not count as final response headers.
  std::function<void(int32_t, Metadata, bool trailers, bool informational)>
      headers;
  // Bytes are borrowed until this callback returns. Consume only after the
  // application has made room to accept further data, not simply on arrival.
  std::function<void(int32_t, std::string_view)> data;
  std::function<void(int32_t)> end;
  // Exactly one terminal transport status, including local resets. RPC status
  // (e.g. grpc-status) is interpreted by the protocol layer, not this engine.
  std::function<void(int32_t, Status)> close;
  // Some queued output was serialized; a blocked writer may retry TryWrite.
  std::function<void(int32_t)> writable;
  // END_STREAM was serialized by Drain, not yet written to a socket.
  std::function<void(int32_t)> send_end;
};
// Incremental HTTP/2 engine with manual flow control and bounded output queues.
// Serialize all access (normally on an Asio strand). Callbacks are synchronous;
// they must not recursively call Receive/Drain or destroy this object. Retain
// closed streams until buffered input is consumed, then explicitly Release.
class Http2StreamSession {
 public:
  static Result<std::unique_ptr<Http2StreamSession>> Create(
      Http2Session::Role role, Http2StreamOptions options,
      Http2StreamEvents events);
  ~Http2StreamSession();
  Http2StreamSession(const Http2StreamSession&) = delete;
  Http2StreamSession& operator=(const Http2StreamSession&) = delete;

  Result<int32_t> StartRequest(Metadata headers);
  Status StartResponse(int32_t stream_id, Metadata headers);
  // Copies up to available queue capacity. Zero bytes accepted means wait for
  // writable/close events and retry. Never silently drops the remaining bytes.
  Result<size_t> TryWrite(int32_t stream_id, std::string_view bytes);
  // Half-closes only after queued data drains. Optional HTTP trailers follow
  // it.
  Status CloseSend(int32_t stream_id, Metadata trailers = {});
  Status Consume(int32_t stream_id, size_t bytes);
  // Release requires close/reset. It discards unread input and returns its
  // connection-level credit, so one abandoned stream cannot starve others.
  Status Release(int32_t stream_id);
  Status Reset(int32_t stream_id, uint32_t error_code = 8);
  Status Shutdown();
  Status Receive(std::string_view bytes);
  Result<std::string> Drain();
  // Terminal connection failure; closes all streams and rejects new operations.
  void Fail(Status status);

 private:
  class Impl;
  explicit Http2StreamSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_HTTP2_STREAM_H_
