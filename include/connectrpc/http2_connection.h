#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_HTTP2_CONNECTION_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_HTTP2_CONNECTION_H_

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "connectrpc/byte_stream.h"
#include "connectrpc/http2_stream.h"

namespace connectrpc {
class Http2ByteStream;
// An application-owned multiplexed HTTP/2 connection. Run must be spawned on
// the application's event loop exactly once. No threads are created. Operations
// on this connection and its streams serialize themselves through its strand.
class Http2Connection {
 public:
  static Result<std::shared_ptr<Http2Connection>> Create(
      asio::ip::tcp::socket socket, Http2Session::Role role,
      Http2StreamOptions options = {});
  // Requires an unhandshaken stream with a configured SSL context. Clients
  // verify hostname and negotiate h2. Servers require h2 ALPN configured by
  // ConfigureTlsContext.
  static Result<std::shared_ptr<Http2Connection>> CreateTls(
      asio::ssl::stream<asio::ip::tcp::socket> socket, Http2Session::Role role,
      std::string hostname, Http2StreamOptions options = {},
      std::chrono::steady_clock::time_point handshake_deadline =
          std::chrono::steady_clock::time_point::max());
  ~Http2Connection();
  Http2Connection(const Http2Connection&) = delete;
  Http2Connection& operator=(const Http2Connection&) = delete;
  asio::awaitable<Status> Run();
  asio::awaitable<Result<std::shared_ptr<Http2ByteStream>>> Open(
      Metadata headers, std::chrono::steady_clock::time_point deadline =
                            std::chrono::steady_clock::time_point::max());
  asio::awaitable<Result<std::shared_ptr<Http2ByteStream>>> Accept();
  // Stops admitting streams, drains existing calls, then closes. At deadline,
  // resets remaining calls. Run returns only after its I/O tasks have joined.
  asio::awaitable<Status> Shutdown(
      std::chrono::steady_clock::time_point deadline);
  asio::awaitable<Status> Cancel();

 private:
  friend class HttpConnection;
  static Result<std::shared_ptr<Http2Connection>> AdoptTls(
      asio::ssl::stream<asio::ip::tcp::socket> socket, Http2Session::Role role,
      Http2StreamOptions options);
  friend class Http2ByteStream;
  class Impl;
  explicit Http2Connection(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};
// Raw HTTP body chunks, not RPC messages. Exactly one reading operation
// (ReadHeaders, Read, or Finish) and one Write may run concurrently. EOF is a
// successful Read with nullopt; Finish returns transport status and trailers.
// Dropping the last handle cancels an unfinished call and releases its buffers.
class Http2ByteStream : public ByteStream {
 public:
  ~Http2ByteStream() override;
  Http2ByteStream(const Http2ByteStream&) = delete;
  Http2ByteStream& operator=(const Http2ByteStream&) = delete;
  asio::any_io_executor executor() const override;
  HttpVersion http_version() const override { return HttpVersion::kHttp2; }
  asio::awaitable<Result<Metadata>> ReadHeaders() override;
  asio::awaitable<Result<std::optional<std::string>>> Read() override;
  asio::awaitable<Status> SendHeaders(Metadata headers) override;
  asio::awaitable<Status> SendResponse(HttpMessage response,
                                       Metadata trailers = {}) override;
  asio::awaitable<Status> Write(std::string bytes) override;
  asio::awaitable<Status> CloseSend(Metadata trailers) override;
  asio::awaitable<Status> CloseSend() { return CloseSend({}); }
  asio::awaitable<Status> FinishSend() override;
  asio::awaitable<Status> DiscardRead() override;
  asio::awaitable<Result<Metadata>> Finish() override;
  asio::awaitable<Result<Metadata>> ReadTrailers() override;
  asio::awaitable<Status> WaitClosed() override;
  asio::awaitable<Status> SetDeadline(
      std::chrono::steady_clock::time_point deadline) override;
  asio::awaitable<Status> Cancel() override;

 private:
  friend class Http2Connection::Impl;
  Http2ByteStream(std::shared_ptr<Http2Connection::Impl> connection,
                  int32_t id);
  std::shared_ptr<Http2Connection::Impl> connection_;
  int32_t id_;
};
// Owns Run and dispatches accepted streams, with bounded concurrent handlers.
// Counts canceled handlers until they join. Connection loss cancels all
// handlers; completion joins them and the connection loop. Handlers must
// cooperate with cancellation. Do not start Run separately. External Shutdown
// drains this loop.
asio::awaitable<Status> ServeHttp2Connection(
    std::shared_ptr<Http2Connection> connection, HttpStreamHandler handler,
    size_t max_handlers = 100);
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_HTTP2_CONNECTION_H_
