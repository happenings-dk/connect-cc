#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_TRANSPORT_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_TRANSPORT_H_

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "connectrpc/byte_stream.h"
#include "connectrpc/http2.h"

namespace connectrpc {
struct TransportOptions {
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  size_t max_body_bytes = 4 * 1024 * 1024;
  size_t max_metadata_bytes = 16 * 1024;
};
using UnaryHandler = std::function<asio::awaitable<HttpMessage>(HttpMessage)>;
// Exchanges a unary HTTP body on an already-opened stream. Request headers
// must already be sent. Reading and writing progress concurrently so an early
// response cannot deadlock a blocked upload. A complete response is retained
// even if the peer stops reading the request. Joins the writer before return.
// Connect unary trailers are encoded in response headers, not HTTP trailers.
asio::awaitable<Result<HttpMessage>> UnaryByteStream(
    std::shared_ptr<ByteStream> stream, std::string body,
    TransportOptions options = {});
// One HTTP/2 prior-knowledge unary call on a connected, exclusively owned
// socket. The socket is closed on return. Cancellation is inherited from the
// coroutine.
asio::awaitable<Result<HttpMessage>> UnaryHttp2(asio::ip::tcp::socket socket,
                                                Metadata headers,
                                                std::string body,
                                                TransportOptions options = {});
// Serves unary messages on one connection. The handler must not block the loop.
// The socket is owned by this operation and closes when the peer disconnects.
asio::awaitable<Status> ServeHttp2(
    asio::ip::tcp::socket socket,
    std::function<HttpMessage(HttpMessage)> handler);
// Handlers run concurrently on the connection strand and receive cancellation
// when their stream resets or the connection closes. Handlers must cooperate
// with cancellation; serving joins all handlers before it returns.
asio::awaitable<Status> ServeHttp2(asio::ip::tcp::socket socket,
                                   UnaryHandler handler);
// TLS context must be configured before constructing the stream. These perform
// the handshake and require h2 ALPN. Unary deadlines include handshake time.
asio::awaitable<Result<HttpMessage>> UnaryHttp2Tls(
    asio::ssl::stream<asio::ip::tcp::socket> stream, std::string hostname,
    Metadata headers, std::string body, TransportOptions options = {});
asio::awaitable<Status> ServeHttp2Tls(
    asio::ssl::stream<asio::ip::tcp::socket> stream,
    std::function<HttpMessage(HttpMessage)> handler,
    std::chrono::steady_clock::time_point handshake_deadline);
asio::awaitable<Status> ServeHttp2Tls(
    asio::ssl::stream<asio::ip::tcp::socket> stream, UnaryHandler handler,
    std::chrono::steady_clock::time_point handshake_deadline);
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_TRANSPORT_H_
