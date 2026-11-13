#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_BYTE_STREAM_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_BYTE_STREAM_H_

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "connectrpc/http2.h"
#include "connectrpc/status.h"

namespace connectrpc {
enum class HttpVersion { kHttp1, kHttp2 };
// Transport boundary for protocol streams. Read returns bounded HTTP body
// chunks and nullopt for EOF; Finish reports transport completion and HTTP
// trailers. Implementations permit one read and one write concurrently and
// serialize state on their executor. Cancellation must wake pending operations.
// Canceling a server Read/ReadHeaders interrupts that operation but leaves the
// response writable. Cancel explicitly aborts the entire stream.
class ByteStream {
 public:
  ByteStream() = default;
  ByteStream(const ByteStream&) = delete;
  ByteStream& operator=(const ByteStream&) = delete;
  virtual ~ByteStream() = default;
  virtual asio::any_io_executor executor() const = 0;
  virtual HttpVersion http_version() const = 0;
  virtual asio::awaitable<Result<Metadata>> ReadHeaders() = 0;
  virtual asio::awaitable<Result<std::optional<std::string>>> Read() = 0;
  virtual asio::awaitable<Status> SendHeaders(Metadata headers) = 0;
  // Server-only complete response. Queues headers with the initial body bytes
  // before yielding when capacity permits; large bodies still obey write
  // limits. Closes the send direction. Use FinishSend to await the socket
  // flush.
  virtual asio::awaitable<Status> SendResponse(HttpMessage response,
                                               Metadata trailers = {}) = 0;
  virtual asio::awaitable<Status> Write(std::string bytes) = 0;
  virtual asio::awaitable<Status> CloseSend(Metadata trailers) = 0;
  // Waits for a queued CloseSend to reach the socket, independently of input.
  virtual asio::awaitable<Status> FinishSend() = 0;
  // Server only, after FinishSend: discards unread request data and ends the
  // request without turning an already-written response into a reset error.
  virtual asio::awaitable<Status> DiscardRead() = 0;
  virtual asio::awaitable<Result<Metadata>> Finish() = 0;
  // Returns HTTP trailers after Read reaches EOF without waiting for the send
  // direction. This permits protocols to finish an early response while an
  // upload is still blocked. Does not consume or cancel the send direction.
  virtual asio::awaitable<Result<Metadata>> ReadTrailers() = 0;
  // Observes transport closure without consuming input or competing with Read.
  // One observer may wait at a time. Canceling it only removes the observer.
  virtual asio::awaitable<Status> WaitClosed() = 0;
  virtual asio::awaitable<Status> SetDeadline(
      std::chrono::steady_clock::time_point deadline) = 0;
  virtual asio::awaitable<Status> Cancel() = 0;
};
using HttpStreamHandler =
    std::function<asio::awaitable<Status>(std::shared_ptr<ByteStream>)>;
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_BYTE_STREAM_H_
