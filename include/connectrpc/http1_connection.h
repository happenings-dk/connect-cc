#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_HTTP1_CONNECTION_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_HTTP1_CONNECTION_H_

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <chrono>
#include <memory>
#include <string>

#include "connectrpc/byte_stream.h"
#include "connectrpc/http1.h"

namespace connectrpc {
struct Http1ConnectionOptions {
  Http1Limits limits;
  // Each direction is bounded separately. Must accommodate a full metadata
  // block or body chunk plus framing. Application-owned Write arguments are
  // not included; Write suspends while progressively copying into this queue.
  size_t max_queued_bytes = 64 * 1024;
};
class Http1ByteStream;
// One active exchange, without pipelining. Applications own and join Run.
// Both TCP and TLS use the same bounded, incremental transport. To reuse the
// connection, finish both directions and release the stream before Open.
// An unfinished call's cancellation closes the connection, since HTTP/1.1 has
// no per-stream reset. Applications provide connection pools when needed.
class Http1Connection {
 public:
  static Result<std::shared_ptr<Http1Connection>> Create(
      asio::ip::tcp::socket socket, Http1Role role,
      Http1ConnectionOptions options = {});
  // Requires an unhandshaken stream. Configure the server context with
  // TlsAlpn::kHttp1. Clients offer http/1.1
  // and verify hostname and trust; an absent ALPN is accepted as HTTP/1.1.
  static Result<std::shared_ptr<Http1Connection>> CreateTls(
      asio::ssl::stream<asio::ip::tcp::socket> socket, Http1Role role,
      std::string hostname, Http1ConnectionOptions options = {},
      std::chrono::steady_clock::time_point handshake_deadline =
          std::chrono::steady_clock::time_point::max());
  ~Http1Connection();
  Http1Connection(const Http1Connection&) = delete;
  Http1Connection& operator=(const Http1Connection&) = delete;
  asio::awaitable<Status> Run();
  asio::awaitable<Result<std::shared_ptr<Http1ByteStream>>> Open(
      Metadata headers, std::chrono::steady_clock::time_point deadline =
                            std::chrono::steady_clock::time_point::max());
  asio::awaitable<Result<std::shared_ptr<Http1ByteStream>>> Accept();
  asio::awaitable<Status> Shutdown(
      std::chrono::steady_clock::time_point deadline);
  asio::awaitable<Status> Cancel();

 private:
  friend class HttpConnection;
  static Result<std::shared_ptr<Http1Connection>> AdoptTls(
      asio::ssl::stream<asio::ip::tcp::socket> socket, Http1Role role,
      Http1ConnectionOptions options);
  friend class Http1ByteStream;
  class Impl;
  explicit Http1Connection(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};
class Http1ByteStream : public ByteStream {
 public:
  ~Http1ByteStream() override;
  Http1ByteStream(const Http1ByteStream&) = delete;
  Http1ByteStream& operator=(const Http1ByteStream&) = delete;
  asio::any_io_executor executor() const override;
  HttpVersion http_version() const override { return HttpVersion::kHttp1; }
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
  friend class Http1Connection::Impl;
  Http1ByteStream(std::shared_ptr<Http1Connection::Impl> connection,
                  uint64_t id);
  std::shared_ptr<Http1Connection::Impl> connection_;
  uint64_t id_;
};
// Owns Run, dispatches one handler at a time, and joins both before returning.
// Cancellation closes the connection and cancels/joins the active handler.
asio::awaitable<Status> ServeHttp1Connection(
    std::shared_ptr<Http1Connection> connection, HttpStreamHandler handler);
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_HTTP1_CONNECTION_H_
