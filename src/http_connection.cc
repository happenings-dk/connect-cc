#include "connectrpc/http_connection.h"

#include <openssl/ssl.h>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <utility>

namespace connectrpc {
namespace {
using Time = std::chrono::steady_clock::time_point;
using Connected = Result<std::shared_ptr<HttpConnection>>;
using TlsSocket = asio::ssl::stream<asio::ip::tcp::socket>;
Status Canceled() { return {Code::kCanceled, "TLS negotiation canceled", {}}; }
Status Expired() {
  return {Code::kDeadlineExceeded, "TLS handshake deadline exceeded", {}};
}
bool ValidRole(HttpConnection::Role role) {
  return role == HttpConnection::Role::kClient ||
         role == HttpConnection::Role::kServer;
}
template <typename Stream>
asio::awaitable<Result<std::shared_ptr<ByteStream>>> EraseStream(
    asio::awaitable<Result<std::shared_ptr<Stream>>> operation) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto result = co_await std::move(operation);
  if (!result.ok())
    co_return Result<std::shared_ptr<ByteStream>>(result.status());
  co_return Result<std::shared_ptr<ByteStream>>(std::move(*result.value()));
}
asio::awaitable<Status> InvalidServing() {
  co_return Status{Code::kInvalidArgument, "invalid HTTP serving options", {}};
}
// The timer callback and handshake coroutine run on one strand. A queued
// successful timer completion cannot close a socket after ownership handoff.
struct HandshakeState {
  TlsSocket socket;
  bool finished = false;
};
}  // namespace
HttpConnection::HttpConnection(Connection connection)
    : connection_(std::move(connection)) {}
Result<std::shared_ptr<HttpConnection>> HttpConnection::Create(
    asio::ip::tcp::socket socket, Role role, HttpVersion version,
    HttpConnectionOptions options) {
  if (!ValidRole(role))
    return Connected(Status{Code::kInvalidArgument, "invalid HTTP role", {}});
  if (version == HttpVersion::kHttp1) {
    auto result = Http1Connection::Create(
        std::move(socket),
        role == Role::kClient ? Http1Role::kClient : Http1Role::kServer,
        options.http1);
    if (!result.ok()) return Connected(result.status());
    return Connected(std::shared_ptr<HttpConnection>(
        new HttpConnection(std::move(*result.value()))));
  }
  if (version != HttpVersion::kHttp2)
    return Connected(
        Status{Code::kInvalidArgument, "invalid HTTP version", {}});
  auto result = Http2Connection::Create(std::move(socket),
                                        role == Role::kClient
                                            ? Http2Session::Role::kClient
                                            : Http2Session::Role::kServer,
                                        options.http2);
  if (!result.ok()) return Connected(result.status());
  return Connected(std::shared_ptr<HttpConnection>(
      new HttpConnection(std::move(*result.value()))));
}
asio::awaitable<Connected> HttpConnection::CreateTls(
    TlsSocket socket, Role role, std::string hostname,
    HttpConnectionOptions options, Time deadline) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none)
    co_return Connected(Canceled());
  auto strand = asio::make_strand(socket.get_executor());
  auto [exception, result] =
      co_await asio::co_spawn(strand,
                              Negotiate(std::move(socket), role,
                                        std::move(hostname), options, deadline),
                              asio::as_tuple(asio::use_awaitable));
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none)
    co_return Connected(Canceled());
  if (exception)
    co_return Connected(
        Status{Code::kInternal, "TLS negotiation task failed", {}});
  co_return std::move(result);
}
asio::awaitable<Connected> HttpConnection::Negotiate(
    TlsSocket socket, Role role, std::string hostname,
    HttpConnectionOptions options, Time deadline) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none)
    co_return Connected(Canceled());
  if (!ValidRole(role) ||
      (options.alpn != TlsAlpn::kHttp1 && options.alpn != TlsAlpn::kHttp2 &&
       options.alpn != TlsAlpn::kHttp2OrHttp1))
    co_return Connected(
        Status{Code::kInvalidArgument, "invalid TLS connection options", {}});
  if (!SSL_in_before(socket.native_handle()))
    co_return Connected(Status{Code::kFailedPrecondition,
                               "TLS stream handshake has already started",
                               {}});
  if (deadline <= std::chrono::steady_clock::now())
    co_return Connected(Expired());
  if (role == Role::kClient) {
    Status configured = ConfigureTlsClient(socket, hostname, options.alpn);
    if (!configured.ok()) co_return Connected(configured);
  }
  auto state =
      std::make_shared<HandshakeState>(HandshakeState{std::move(socket)});
  asio::steady_timer timer(co_await asio::this_coro::executor);
  if (deadline != Time::max()) {
    timer.expires_at(deadline);
    timer.async_wait(
        [weak = std::weak_ptr<HandshakeState>(state)](asio::error_code error) {
          if (error) return;
          if (auto state = weak.lock(); state && !state->finished) {
            asio::error_code ignored;
            state->socket.lowest_layer().close(ignored);
          }
        });
  }
  auto [error] = co_await state->socket.async_handshake(
      role == Role::kClient ? asio::ssl::stream_base::client
                            : asio::ssl::stream_base::server,
      asio::as_tuple(asio::use_awaitable));
  state->finished = true;
  timer.cancel();
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none)
    co_return Connected(Canceled());
  if (deadline <= std::chrono::steady_clock::now())
    co_return Connected(Expired());
  if (error)
    co_return Connected(Status{Code::kUnavailable, error.message(), {}});
  auto protocol = NegotiatedTlsProtocol(state->socket, options.alpn);
  if (!protocol.ok()) co_return Connected(protocol.status());
  if (*protocol.value() == TlsProtocol::kHttp1) {
    auto result = Http1Connection::AdoptTls(
        std::move(state->socket),
        role == Role::kClient ? Http1Role::kClient : Http1Role::kServer,
        options.http1);
    if (!result.ok()) co_return Connected(result.status());
    co_return Connected(std::shared_ptr<HttpConnection>(
        new HttpConnection(std::move(*result.value()))));
  }
  auto result = Http2Connection::AdoptTls(std::move(state->socket),
                                          role == Role::kClient
                                              ? Http2Session::Role::kClient
                                              : Http2Session::Role::kServer,
                                          options.http2);
  if (!result.ok()) co_return Connected(result.status());
  co_return Connected(std::shared_ptr<HttpConnection>(
      new HttpConnection(std::move(*result.value()))));
}
HttpVersion HttpConnection::http_version() const {
  return std::holds_alternative<std::shared_ptr<Http1Connection>>(connection_)
             ? HttpVersion::kHttp1
             : HttpVersion::kHttp2;
}
asio::awaitable<Status> HttpConnection::Run() {
  return std::visit([](auto& connection) { return connection->Run(); },
                    connection_);
}
asio::awaitable<Result<std::shared_ptr<ByteStream>>> HttpConnection::Open(
    Metadata headers, Time deadline) {
  return std::visit(
      [&](auto& connection) {
        return EraseStream(connection->Open(std::move(headers), deadline));
      },
      connection_);
}
asio::awaitable<Result<std::shared_ptr<ByteStream>>> HttpConnection::Accept() {
  return std::visit(
      [](auto& connection) { return EraseStream(connection->Accept()); },
      connection_);
}
asio::awaitable<Status> HttpConnection::Shutdown(Time deadline) {
  return std::visit(
      [deadline](auto& connection) { return connection->Shutdown(deadline); },
      connection_);
}
asio::awaitable<Status> HttpConnection::Cancel() {
  return std::visit([](auto& connection) { return connection->Cancel(); },
                    connection_);
}
asio::awaitable<Status> ServeHttpConnection(
    std::shared_ptr<HttpConnection> connection, HttpStreamHandler handler,
    size_t max_handlers) {
  if (!connection || !handler || max_handlers == 0) return InvalidServing();
  if (auto* http1 = std::get_if<std::shared_ptr<Http1Connection>>(
          &connection->connection_))
    return ServeHttp1Connection(*http1, std::move(handler));
  return ServeHttp2Connection(
      std::get<std::shared_ptr<Http2Connection>>(connection->connection_),
      std::move(handler), max_handlers);
}
}  // namespace connectrpc
