#include <algorithm>
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <memory>
#include <utility>

#include "connectrpc/http2_connection.h"
#include "connectrpc/transport.h"

namespace connectrpc {
namespace {
using HttpResult = Result<HttpMessage>;
Status Canceled() { return {Code::kCanceled, "call canceled", {}}; }
asio::awaitable<Status> WriteBody(std::shared_ptr<ByteStream> stream,
                                  std::string body) {
  co_await asio::this_coro::throw_if_cancelled(false);
  Status status = co_await stream->Write(std::move(body));
  if (status.ok()) status = co_await stream->CloseSend({});
  // NO_ERROR may stop an upload after a complete response. Keep that response
  // readable; all other write failures wake the reader by aborting the stream.
  if (!status.ok() && status.code != Code::kFailedPrecondition)
    co_await stream->Cancel();
  co_return status;
}
asio::awaitable<HttpResult> ReadBody(std::shared_ptr<ByteStream> stream,
                                     TransportOptions options) {
  auto headers = co_await stream->ReadHeaders();
  if (!headers.ok()) co_return HttpResult(headers.status());
  size_t remaining = options.max_metadata_bytes;
  for (const auto& [name, value] : *headers.value()) {
    if (remaining < 32 || name.size() > remaining - 32 ||
        value.size() > remaining - 32 - name.size())
      co_return HttpResult(
          {Code::kResourceExhausted, "response metadata exceeds limit", {}});
    remaining -= 32 + name.size() + value.size();
  }
  HttpMessage response;
  response.headers = std::move(*headers.value());
  for (;;) {
    auto bytes = co_await stream->Read();
    if (!bytes.ok()) co_return HttpResult(bytes.status());
    if (!*bytes.value()) break;
    if ((**bytes.value()).size() >
        options.max_body_bytes - response.body.size())
      co_return HttpResult(
          {Code::kResourceExhausted, "response exceeds limit", {}});
    response.body.append(**bytes.value());
  }
  co_return HttpResult(std::move(response));
}
// Runs on one strand. No writer survives the operation, including cancellation
// and a server response received before the upload completes.
asio::awaitable<HttpResult> ExchangeBody(std::shared_ptr<ByteStream> stream,
                                         std::string body,
                                         TransportOptions options) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = co_await asio::this_coro::executor;
  Status rejected;
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none)
    rejected = Canceled();
  else if (options.deadline <= std::chrono::steady_clock::now())
    rejected = {Code::kDeadlineExceeded, "deadline exceeded", {}};
  else if (body.size() > options.max_body_bytes)
    rejected = {Code::kResourceExhausted, "request exceeds limit", {}};
  if (rejected.ok()) rejected = co_await stream->SetDeadline(options.deadline);
  if (!rejected.ok()) {
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());
    co_await stream->Cancel();
    co_return HttpResult(rejected);
  }
  struct State {
    explicit State(asio::any_io_executor executor) : joined(executor, 1) {}
    asio::experimental::channel<void(asio::error_code)> joined;
    Status written;
  };
  auto state = std::make_shared<State>(executor);
  asio::co_spawn(executor, WriteBody(stream, std::move(body)),
                 [state](auto exception, Status written) {
                   state->written =
                       exception ? Status{Code::kInternal, "writer failed", {}}
                                 : std::move(written);
                   state->joined.try_send(asio::error_code{});
                 });
  auto response = co_await ReadBody(stream, options);
  bool canceled = (co_await asio::this_coro::cancellation_state).cancelled() !=
                  asio::cancellation_type::none;
  co_await asio::this_coro::reset_cancellation_state(
      asio::disable_cancellation());
  // Connect unary uses prefixed headers for trailers. EOF is authoritative;
  // do not wait for an unfinished request before preserving its response.
  co_await stream->Cancel();
  co_await state->joined.async_receive(asio::as_tuple(asio::use_awaitable));
  if (canceled) co_return HttpResult(Canceled());
  if (!response.ok() && response.status().code == Code::kCanceled &&
      !state->written.ok())
    co_return HttpResult(state->written);
  co_return response;
}
asio::awaitable<HttpResult> RunUnaryConnection(
    std::shared_ptr<Http2Connection> connection, Metadata headers,
    std::string body, TransportOptions options) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = co_await asio::this_coro::executor;
  asio::experimental::channel<void(asio::error_code)> joined(executor, 1);
  asio::co_spawn(executor, connection->Run(), [&](auto exception, Status) {
    (void)exception;
    joined.try_send(asio::error_code{});
  });
  HttpResult result;
  auto stream = co_await connection->Open(std::move(headers), options.deadline);
  if (stream.ok())
    result =
        co_await UnaryByteStream(*stream.value(), std::move(body), options);
  else
    result = HttpResult(stream.status());
  co_await asio::this_coro::reset_cancellation_state(
      asio::disable_cancellation());
  if (result.ok())
    co_await connection->Shutdown(
        std::min(options.deadline,
                 std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  else
    co_await connection->Cancel();
  co_await joined.async_receive(asio::as_tuple(asio::use_awaitable));
  co_return result;
}
asio::awaitable<HttpResult> DispatchConnection(
    Result<std::shared_ptr<Http2Connection>> connection, Metadata headers,
    std::string body, TransportOptions options) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!connection.ok()) co_return HttpResult(connection.status());
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none) {
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());
    co_await (*connection.value())->Cancel();
    co_return HttpResult(Canceled());
  }
  auto [exception, result] = co_await asio::co_spawn(
      asio::make_strand(co_await asio::this_coro::executor),
      RunUnaryConnection(*connection.value(), std::move(headers),
                         std::move(body), options),
      asio::as_tuple(asio::use_awaitable));
  if (exception)
    co_return HttpResult({Code::kInternal, "unary connection failed", {}});
  co_return result;
}
}  // namespace
asio::awaitable<Result<HttpMessage>> UnaryByteStream(
    std::shared_ptr<ByteStream> stream, std::string body,
    TransportOptions options) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!stream)
    co_return HttpResult({Code::kInvalidArgument, "missing byte stream", {}});
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none) {
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());
    co_await stream->Cancel();
    co_return HttpResult(Canceled());
  }
  auto executor = asio::make_strand(stream->executor());
  auto [exception, result] = co_await asio::co_spawn(
      executor, ExchangeBody(std::move(stream), std::move(body), options),
      asio::as_tuple(asio::use_awaitable));
  if (exception)
    co_return HttpResult({Code::kInternal, "unary exchange failed", {}});
  co_return result;
}
asio::awaitable<Result<HttpMessage>> UnaryHttp2(asio::ip::tcp::socket socket,
                                                Metadata headers,
                                                std::string body,
                                                TransportOptions options) {
  Http2StreamOptions limits;
  limits.max_metadata_bytes = options.max_metadata_bytes;
  return DispatchConnection(
      Http2Connection::Create(std::move(socket), Http2Session::Role::kClient,
                              limits),
      std::move(headers), std::move(body), options);
}
asio::awaitable<Result<HttpMessage>> UnaryHttp2Tls(
    asio::ssl::stream<asio::ip::tcp::socket> stream, std::string hostname,
    Metadata headers, std::string body, TransportOptions options) {
  Http2StreamOptions limits;
  limits.max_metadata_bytes = options.max_metadata_bytes;
  return DispatchConnection(
      Http2Connection::CreateTls(std::move(stream), Http2Session::Role::kClient,
                                 std::move(hostname), limits, options.deadline),
      std::move(headers), std::move(body), options);
}
}  // namespace connectrpc
