#include <asio/as_tuple.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <string>
#include <type_traits>
#include <utility>

#include "conformance/adapter.h"
#include "connectrpc/tls.h"

namespace connectrpc::conformance_adapter {
namespace {
namespace pb = conformance::v1;
template <typename Input, typename Output>
asio::awaitable<pb::ClientCompatResponse> Invoke(
    const pb::ClientCompatRequest& request, Client& client) {
  pb::ClientCompatResponse result;
  result.set_test_name(request.test_name());
  Input input;
  if (request.request_messages_size() != 1 ||
      !request.request_messages(0).UnpackTo(&input)) {
    result.mutable_error()->set_message(
        "unary request needs one matching message");
    co_return result;
  }
  CallOptions options;
  options.protocol =
      request.protocol() == pb::PROTOCOL_GRPC_WEB ? Protocol::kGrpcWeb
      : request.protocol() == pb::PROTOCOL_GRPC   ? Protocol::kGrpc
                                                  : Protocol::kConnect;
  options.codec =
      request.codec() == pb::CODEC_JSON ? Codec::kJson : Codec::kProtobuf;
  options.use_get = request.use_get_http_method();
  MethodOptions method;
  if (request.method() == "IdempotentUnary")
    method.idempotency = Idempotency::kNoSideEffects;
  options.compress = request.compression() == pb::COMPRESSION_GZIP;
  options.headers = ReadHeaders(request.request_headers());
  if (request.message_receive_limit())
    options.transport.max_body_bytes = request.message_receive_limit();
  else
    options.transport.max_body_bytes = 16 * 1024 * 1024;
  if (request.has_timeout_ms())
    options.transport.deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(request.timeout_ms());
  auto response = co_await client.Unary<Input, Output>(
      "/" + request.service() + "/" + request.method(), std::move(input),
      options, method);
  auto& output = *result.mutable_response();
  WriteHeaders(response.headers, *output.mutable_response_headers());
  WriteHeaders(response.trailers, *output.mutable_response_trailers());
  if (!response.status.ok()) {
    auto& error = *output.mutable_error();
    error.set_code(static_cast<pb::Code>(response.status.code));
    error.set_message(response.status.message);
    for (const auto& detail : response.status.details) {
      auto* any = error.add_details();
      any->set_type_url("type.googleapis.com/" + detail.type);
      any->set_value(detail.value);
    }
  } else if constexpr (!std::is_same_v<Output, pb::UnimplementedResponse>) {
    if (response.message) *output.add_payloads() = response.message->payload();
  }
  co_return result;
}
}  // namespace
asio::awaitable<pb::ClientCompatResponse> InvokeUnaryClient(
    const pb::ClientCompatRequest& request, Client& client) {
  if (request.method() == "Unary")
    co_return co_await Invoke<pb::UnaryRequest, pb::UnaryResponse>(request,
                                                                   client);
  if (request.method() == "IdempotentUnary")
    co_return co_await Invoke<pb::IdempotentUnaryRequest,
                              pb::IdempotentUnaryResponse>(request, client);
  if (request.method() == "Unimplemented")
    co_return co_await Invoke<pb::UnimplementedRequest,
                              pb::UnimplementedResponse>(request, client);
  pb::ClientCompatResponse result;
  result.set_test_name(request.test_name());
  result.mutable_error()->set_message("unsupported unary method");
  co_return result;
}
asio::awaitable<conformance::v1::ClientCompatResponse> RunClient(
    pb::ClientCompatRequest request) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = co_await asio::this_coro::executor;
  if (request.stream_type() != pb::STREAM_TYPE_UNARY ||
      request.protocol() != pb::PROTOCOL_CONNECT ||
      request.http_version() == pb::HTTP_VERSION_1)
    co_return co_await RunStreamingClient(std::move(request));
  pb::ClientCompatResponse failure;
  failure.set_test_name(request.test_name());
  if (request.protocol() != pb::PROTOCOL_CONNECT ||
      request.http_version() != pb::HTTP_VERSION_2 ||
      request.stream_type() != pb::STREAM_TYPE_UNARY ||
      request.has_raw_request()) {
    failure.mutable_error()->set_message(
        "unsupported client capability in profile");
    co_return failure;
  }
  asio::ssl::context tls(asio::ssl::context::tls_client);
  if (!request.server_tls_cert().empty()) {
    TlsOptions options;
    options.trust_pem = request.server_tls_cert();
    options.certificate_pem = request.client_tls_creds().cert();
    options.private_key_pem = request.client_tls_creds().key();
    Status status = ConfigureTlsContext(tls, false, options);
    if (!status.ok()) {
      failure.mutable_error()->set_message(status.message);
      co_return failure;
    }
  }
  Client client([&request, &tls, executor](HttpMessage message,
                                           TransportOptions options)
                    -> asio::awaitable<Result<HttpMessage>> {
    co_await asio::this_coro::throw_if_cancelled(false);
    asio::ip::tcp::resolver resolver(executor);
    auto [resolve_error, endpoints] = co_await resolver.async_resolve(
        request.host(), std::to_string(request.port()),
        asio::as_tuple(asio::use_awaitable));
    if (resolve_error)
      co_return Result<HttpMessage>(
          {Code::kUnavailable, resolve_error.message(), {}});
    asio::ip::tcp::socket socket(executor);
    auto [error, endpoint] = co_await asio::async_connect(
        socket, endpoints, asio::as_tuple(asio::use_awaitable));
    (void)endpoint;
    if (error)
      co_return Result<HttpMessage>({error == asio::error::operation_aborted
                                         ? Code::kCanceled
                                         : Code::kUnavailable,
                                     error.message(),
                                     {}});
    bool secure = !request.server_tls_cert().empty();
    message.headers.insert(
        message.headers.begin(),
        {{":scheme", secure ? "https" : "http"},
         {":authority",
          request.host() + ":" + std::to_string(request.port())}});
    if (secure)
      co_return co_await UnaryHttp2Tls(
          asio::ssl::stream<asio::ip::tcp::socket>(std::move(socket), tls),
          request.host(), std::move(message.headers), std::move(message.body),
          options);
    co_return co_await UnaryHttp2(std::move(socket), std::move(message.headers),
                                  std::move(message.body), options);
  });
  if (request.method() == "Unary")
    co_return co_await Invoke<pb::UnaryRequest, pb::UnaryResponse>(request,
                                                                   client);
  if (request.method() == "IdempotentUnary")
    co_return co_await Invoke<pb::IdempotentUnaryRequest,
                              pb::IdempotentUnaryResponse>(request, client);
  if (request.method() == "Unimplemented")
    co_return co_await Invoke<pb::UnimplementedRequest,
                              pb::UnimplementedResponse>(request, client);
  failure.mutable_error()->set_message("unsupported conformance method: " +
                                       request.method());
  co_return failure;
}
}  // namespace connectrpc::conformance_adapter
