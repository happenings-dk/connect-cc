#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_HTTP_CONNECTION_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_HTTP_CONNECTION_H_

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <variant>

#include "connectrpc/byte_stream.h"
#include "connectrpc/http1_connection.h"
#include "connectrpc/http2_connection.h"
#include "connectrpc/tls.h"

namespace connectrpc {
struct HttpConnectionOptions {
  Http1ConnectionOptions http1;
  Http2StreamOptions http2;
  // TLS only. h2 is preferred; absent ALPN permits HTTP/1.1 in dual mode.
  TlsAlpn alpn = TlsAlpn::kHttp2OrHttp1;
};
// Common connection API over the existing bounded HTTP engines. Applications
// still own Run and the event loop. HTTP/1.1 admits one call at a time; HTTP/2
// multiplexes. No pool, retries, extra connections, or worker threads are
// added.
class HttpConnection {
 public:
  enum class Role { kClient, kServer };
  // Cleartext requires an explicit version; HTTP/2 uses prior knowledge.
  static Result<std::shared_ptr<HttpConnection>> Create(
      asio::ip::tcp::socket socket, Role role, HttpVersion version,
      HttpConnectionOptions options = {});
  // Takes an already-connected, unhandshaken TLS stream. Completes the TLS
  // handshake before returning the matching HTTP engine. Clients verify chain
  // and hostname and offer options.alpn. Configure the server context's ALPN
  // policy with ConfigureTlsContext before constructing the stream. The
  // configured context must outlive the connection and all its streams.
  // Failure/cancellation closes the supplied socket; no downgrade retry occurs.
  static asio::awaitable<Result<std::shared_ptr<HttpConnection>>> CreateTls(
      asio::ssl::stream<asio::ip::tcp::socket> socket, Role role,
      std::string hostname, HttpConnectionOptions options = {},
      std::chrono::steady_clock::time_point handshake_deadline =
          std::chrono::steady_clock::time_point::max());
  HttpConnection(const HttpConnection&) = delete;
  HttpConnection& operator=(const HttpConnection&) = delete;
  HttpVersion http_version() const;
  asio::awaitable<Status> Run();
  asio::awaitable<Result<std::shared_ptr<ByteStream>>> Open(
      Metadata headers, std::chrono::steady_clock::time_point deadline =
                            std::chrono::steady_clock::time_point::max());
  asio::awaitable<Result<std::shared_ptr<ByteStream>>> Accept();
  asio::awaitable<Status> Shutdown(
      std::chrono::steady_clock::time_point deadline);
  asio::awaitable<Status> Cancel();

 private:
  using Connection = std::variant<std::shared_ptr<Http1Connection>,
                                  std::shared_ptr<Http2Connection>>;
  explicit HttpConnection(Connection connection);
  static asio::awaitable<Result<std::shared_ptr<HttpConnection>>> Negotiate(
      asio::ssl::stream<asio::ip::tcp::socket> socket, Role role,
      std::string hostname, HttpConnectionOptions options,
      std::chrono::steady_clock::time_point deadline);
  friend asio::awaitable<Status> ServeHttpConnection(
      std::shared_ptr<HttpConnection>, HttpStreamHandler, size_t);
  Connection connection_;
};
// Owns Run, dispatches and joins handlers using the selected HTTP engine.
// HTTP/1.1 remains sequential even if max_handlers exceeds one. Do not start
// Run separately. max_handlers must be positive for either HTTP version.
asio::awaitable<Status> ServeHttpConnection(
    std::shared_ptr<HttpConnection> connection, HttpStreamHandler handler,
    size_t max_handlers = 100);
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_HTTP_CONNECTION_H_
