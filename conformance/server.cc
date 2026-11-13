#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <cstdio>
#include <memory>
#include <utility>

#include "conformance/adapter.h"
#include "connectrpc/http1_connection.h"
#include "connectrpc/http2_connection.h"
#include "connectrpc/tls.h"

namespace connectrpc::conformance_adapter {
namespace {
namespace pb = conformance::v1;
template <typename Input, typename Output>
asio::awaitable<Response<Output>> Unary(Request<Input> request) {
  co_await asio::this_coro::throw_if_cancelled(false);
  pb::ConformancePayload::RequestInfo info;
  WriteHeaders(request.headers, *info.mutable_request_headers());
  if (!info.add_requests()->PackFrom(request.message)) {
    Response<Output> failed;
    failed.status = {Code::kInternal, "cannot pack request info", {}};
    co_return failed;
  }
  if (request.deadline != std::chrono::steady_clock::time_point::max())
    info.set_timeout_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                            request.deadline - std::chrono::steady_clock::now())
                            .count());
  const auto& definition = request.message.response_definition();
  Response<Output> response;
  response.headers = ReadHeaders(definition.response_headers());
  response.trailers = ReadHeaders(definition.response_trailers());
  if (definition.has_error()) {
    response.status.code = static_cast<Code>(definition.error().code());
    response.status.message = definition.error().message();
    for (const auto& detail : definition.error().details()) {
      std::string type = detail.type_url();
      type.erase(0, type.find_last_of('/') + 1);
      response.status.details.push_back({std::move(type), detail.value()});
    }
    std::string bytes;
    if (!info.SerializeToString(&bytes)) {
      response.status = {Code::kInternal, "cannot encode request info", {}};
      co_return response;
    }
    response.status.details.push_back(
        {std::string(info.GetTypeName()), std::move(bytes)});
  } else {
    response.message.emplace();
    auto& payload = *response.message->mutable_payload();
    payload.set_data(definition.response_data());
    *payload.mutable_request_info() = std::move(info);
  }
  if (definition.response_delay_ms()) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(
        std::chrono::milliseconds(definition.response_delay_ms()));
    auto [error] =
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    if (error) response.status = {Code::kCanceled, error.message(), {}};
  }
  co_return response;
}
}  // namespace
asio::awaitable<Status> RunServer(pb::ServerCompatRequest request) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if ((request.protocol() != pb::PROTOCOL_CONNECT &&
       request.protocol() != pb::PROTOCOL_GRPC &&
       request.protocol() != pb::PROTOCOL_GRPC_WEB) ||
      (request.http_version() != pb::HTTP_VERSION_2 &&
       request.http_version() != pb::HTTP_VERSION_1))
    co_return Status{
        Code::kInvalidArgument, "unsupported server capability in profile", {}};
  auto tls =
      std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
  if (request.use_tls()) {
    TlsOptions options;
    options.alpn = request.http_version() == pb::HTTP_VERSION_1
                       ? TlsAlpn::kHttp1
                       : TlsAlpn::kHttp2;
    options.certificate_pem = request.server_creds().cert();
    options.private_key_pem = request.server_creds().key();
    options.trust_pem = request.client_tls_cert();
    options.require_client_certificate = !options.trust_pem.empty();
    Status status = ConfigureTlsContext(*tls, true, options);
    if (!status.ok()) co_return status;
  }
  auto server = std::make_shared<Server>();
  Status streaming_status = RegisterStreamingService(*server);
  if (!streaming_status.ok()) co_return streaming_status;
  Status status = server->RegisterUnary<pb::UnaryRequest, pb::UnaryResponse>(
      "/connectrpc.conformance.v1.ConformanceService/Unary",
      Unary<pb::UnaryRequest, pb::UnaryResponse>);
  if (!status.ok()) co_return status;
  status = server->RegisterUnary<pb::IdempotentUnaryRequest,
                                 pb::IdempotentUnaryResponse>(
      "/connectrpc.conformance.v1.ConformanceService/IdempotentUnary",
      Unary<pb::IdempotentUnaryRequest, pb::IdempotentUnaryResponse>,
      {Idempotency::kNoSideEffects});
  if (!status.ok()) co_return status;
  auto executor = co_await asio::this_coro::executor;
  status =
      server
          ->RegisterUnary<pb::UnimplementedRequest, pb::UnimplementedResponse>(
              "/connectrpc.conformance.v1.ConformanceService/Unimplemented",
              [](Request<pb::UnimplementedRequest>)
                  -> asio::awaitable<Response<pb::UnimplementedResponse>> {
                Response<pb::UnimplementedResponse> response;
                response.status = {Code::kUnimplemented, "not implemented", {}};
                co_return response;
              });
  if (!status.ok()) co_return status;
  asio::ip::tcp::acceptor acceptor(executor);
  asio::error_code error;
  acceptor.open(asio::ip::tcp::v4(), error);
  if (!error) acceptor.bind({asio::ip::address_v4::loopback(), 0}, error);
  if (!error) acceptor.listen(asio::socket_base::max_listen_connections, error);
  if (error) co_return Status{Code::kUnavailable, error.message(), {}};
  auto endpoint = acceptor.local_endpoint(error);
  if (error) co_return Status{Code::kUnavailable, error.message(), {}};
  pb::ServerCompatResponse ready;
  ready.set_host("127.0.0.1");
  ready.set_port(endpoint.port());
  if (request.use_tls()) ready.set_pem_cert(request.server_creds().cert());
  status = WriteFrame(ready);
  if (!status.ok()) co_return status;
  size_t limit = request.message_receive_limit()
                     ? request.message_receive_limit()
                     : 16 * 1024 * 1024;
  HttpStreamHandler handler =
      [server,
       limit](std::shared_ptr<ByteStream> stream) -> asio::awaitable<Status> {
    StreamServerOptions options;
    options.limits.max_message_bytes = 16 * 1024 * 1024;
    options.limits.max_decompressed_bytes = limit;
    co_return co_await server->HandleHttpStream(std::move(stream), options);
  };
  // Each connection retains its TLS context and service, including on accept
  // failure.
  for (;;) {
    auto [accept_error, socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (accept_error)
      co_return Status{Code::kUnavailable, accept_error.message(), {}};
    if (request.http_version() == pb::HTTP_VERSION_1) {
      auto connection =
          request.use_tls()
              ? Http1Connection::CreateTls(
                    asio::ssl::stream<asio::ip::tcp::socket>(std::move(socket),
                                                             *tls),
                    Http1Role::kServer, "", {},
                    std::chrono::steady_clock::now() + std::chrono::seconds(10))
              : Http1Connection::Create(std::move(socket), Http1Role::kServer);
      if (!connection.ok()) co_return connection.status();
      asio::co_spawn(
          executor, ServeHttp1Connection(*connection.value(), handler),
          [tls](auto exception, Status connection_status) {
            if (exception)
              std::fprintf(stderr, "unexpected HTTP/1.1 server failure\n");
            (void)connection_status;
          });
      continue;
    }
    auto connection =
        request.use_tls()
            ? Http2Connection::CreateTls(
                  asio::ssl::stream<asio::ip::tcp::socket>(std::move(socket),
                                                           *tls),
                  Http2Session::Role::kServer, "", {},
                  std::chrono::steady_clock::now() + std::chrono::seconds(10))
            : Http2Connection::Create(std::move(socket),
                                      Http2Session::Role::kServer);
    if (!connection.ok()) co_return connection.status();
    asio::co_spawn(executor, ServeHttp2Connection(*connection.value(), handler),
                   [tls](auto exception, Status connection_status) {
                     if (exception)
                       std::fprintf(stderr,
                                    "unexpected server connection failure\n");
                     // Peers cancel many conformance cases intentionally.
                     (void)connection_status;
                   });
  }
}
}  // namespace connectrpc::conformance_adapter
