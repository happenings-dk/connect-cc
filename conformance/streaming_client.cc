#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "conformance/adapter.h"
#include "connectrpc/http1_connection.h"
#include "connectrpc/http2_connection.h"
#include "connectrpc/tls.h"

namespace connectrpc::conformance_adapter {
namespace {
namespace pb = conformance::v1;
void RecordError(const Status& status, pb::ClientResponseResult& response) {
  if (status.ok()) return;
  auto& error = *response.mutable_error();
  error.set_code(static_cast<pb::Code>(status.code));
  error.set_message(status.message);
  for (const auto& detail : status.details) {
    auto* any = error.add_details();
    any->set_type_url("type.googleapis.com/" + detail.type);
    any->set_value(detail.value);
  }
}
asio::awaitable<Status> Delay(uint32_t milliseconds) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!milliseconds) co_return Status{};
  asio::steady_timer timer(co_await asio::this_coro::executor);
  timer.expires_after(std::chrono::milliseconds(milliseconds));
  auto [error] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
  if (error) co_return Status{Code::kCanceled, error.message(), {}};
  co_return Status{};
}
template <typename Input, typename Output>
asio::awaitable<pb::ClientCompatResponse> InvokeStreaming(
    const pb::ClientCompatRequest& request, Client& client,
    std::shared_ptr<asio::cancellation_signal> cancellation) {
  co_await asio::this_coro::throw_if_cancelled(false);
  pb::ClientCompatResponse result;
  result.set_test_name(request.test_name());
  std::vector<Input> inputs;
  for (const auto& packed : request.request_messages()) {
    Input input;
    if (!packed.UnpackTo(&input)) {
      result.mutable_error()->set_message("request message type mismatch");
      co_return result;
    }
    inputs.push_back(std::move(input));
  }
  bool server_stream = request.stream_type() == pb::STREAM_TYPE_SERVER_STREAM;
  bool client_stream = request.stream_type() == pb::STREAM_TYPE_CLIENT_STREAM;
  bool full_duplex =
      request.stream_type() == pb::STREAM_TYPE_FULL_DUPLEX_BIDI_STREAM;
  if (server_stream && inputs.size() != 1) {
    result.mutable_error()->set_message(
        "server streaming requires one request");
    co_return result;
  }
  auto& response = *result.mutable_response();
  StreamCallOptions options;
  options.protocol =
      request.protocol() == pb::PROTOCOL_GRPC_WEB ? Protocol::kGrpcWeb
      : request.protocol() == pb::PROTOCOL_GRPC   ? Protocol::kGrpc
                                                  : Protocol::kConnect;
  options.codec =
      request.codec() == pb::CODEC_JSON ? Codec::kJson : Codec::kProtobuf;
  options.compress = request.compression() == pb::COMPRESSION_GZIP;
  options.headers = ReadHeaders(request.request_headers());
  options.limits.max_decompressed_bytes = request.message_receive_limit()
                                              ? request.message_receive_limit()
                                              : 16 * 1024 * 1024;
  options.limits.max_message_bytes = 16 * 1024 * 1024;
  if (request.has_timeout_ms())
    options.deadline = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(request.timeout_ms());
  std::string procedure = "/" + request.service() + "/" + request.method();
  using Stream = TypedStream<Input, Output>;
  Result<Stream> opened;
  if (server_stream) {
    Status delay = co_await Delay(request.request_delay_ms());
    if (!delay.ok()) {
      RecordError(delay, response);
      co_return result;
    }
    opened = co_await client.ServerStream<Input, Output>(
        procedure, inputs.front(), options);
  } else if (client_stream) {
    opened = co_await client.ClientStream<Input, Output>(procedure, options);
  } else {
    opened = co_await client.BidiStream<Input, Output>(procedure, options);
  }
  if (!opened.ok()) {
    RecordError(opened.status(), response);
    co_return result;
  }
  Stream stream = std::move(*opened.value());
  Status read_status;
  bool eof = false;
  bool have_headers = false;
  auto receive = [&]() -> asio::awaitable<bool> {
    if (!have_headers) {
      auto headers = co_await stream.ReadHeaders();
      if (!headers.ok()) {
        read_status = headers.status();
        co_return false;
      }
      WriteHeaders(*headers.value(), *response.mutable_response_headers());
      have_headers = true;
    }
    auto next = co_await stream.Read();
    if (!next.ok()) {
      read_status = next.status();
      co_return false;
    }
    if (!*next.value()) {
      eof = true;
      co_return false;
    }
    *response.add_payloads() = (**next.value()).payload();
    if (request.has_cancel() && request.cancel().has_after_num_responses() &&
        response.payloads_size() ==
            static_cast<int>(request.cancel().after_num_responses()))
      co_await stream.Cancel();
    co_return true;
  };
  if (!server_stream) {
    for (size_t i = 0; i < inputs.size(); ++i) {
      Status sent = co_await Delay(request.request_delay_ms());
      if (sent.ok()) sent = co_await stream.Write(inputs[i]);
      if (!sent.ok()) {
        response.set_num_unsent_requests(static_cast<int>(inputs.size() - i));
        break;  // Read the response to preserve an early server RPC error.
      }
      if (full_duplex && !co_await receive()) break;
    }
    if (request.has_cancel() && request.cancel().has_before_close_send())
      co_await stream.Cancel();
    else
      co_await stream.CloseSend();
  }
  asio::steady_timer timer(co_await asio::this_coro::executor);
  if (request.has_cancel() && !request.cancel().has_before_close_send() &&
      !request.cancel().has_after_num_responses()) {
    if (client_stream) {
      timer.expires_after(
          std::chrono::milliseconds(request.cancel().after_close_send_ms()));
      timer.async_wait([cancellation](asio::error_code error) {
        if (!error) cancellation->emit(asio::cancellation_type::all);
      });
    } else {
      co_await Delay(request.cancel().after_close_send_ms());
      co_await stream.Cancel();
    }
  }
  while (!eof && read_status.ok() && co_await receive()) {
  }
  timer.cancel();
  if (eof) {
    auto completion = co_await stream.Finish();
    WriteHeaders(completion.trailers, *response.mutable_response_trailers());
    RecordError(completion.status, response);
  } else {
    RecordError(read_status, response);
    co_await stream.Cancel();
  }
  // Client-streaming has one unary result: a provisional message is not a
  // result until cardinality, terminal framing, and final RPC status succeed.
  if (client_stream && response.has_error()) response.clear_payloads();
  co_return result;
}
template <typename Connection>
asio::awaitable<pb::ClientCompatResponse> InvokeOnConnection(
    const pb::ClientCompatRequest& request,
    std::shared_ptr<Connection> transport, bool secure) {
  auto executor = co_await asio::this_coro::executor;
  pb::ClientCompatResponse result;
  result.set_test_name(request.test_name());

  asio::experimental::channel<void(asio::error_code)> joined(executor, 1);
  asio::co_spawn(executor, transport->Run(), [&](auto exception, Status) {
    (void)exception;
    joined.try_send(asio::error_code{});
  });
  Client client(
      {},
      [transport, &request, secure](Metadata headers, TransportOptions options)
          -> asio::awaitable<Result<std::shared_ptr<ByteStream>>> {
        headers.insert(headers.begin(),
                       {{":scheme", secure ? "https" : "http"},
                        {":authority", request.host() + ":" +
                                           std::to_string(request.port())}});
        auto stream =
            co_await transport->Open(std::move(headers), options.deadline);
        if (!stream.ok())
          co_return Result<std::shared_ptr<ByteStream>>(stream.status());
        co_return Result<std::shared_ptr<ByteStream>>(*stream.value());
      });
  auto signal = std::make_shared<asio::cancellation_signal>();
  auto parent = co_await asio::this_coro::cancellation_state;
  if (parent.slot().is_connected())
    parent.slot().assign(
        [signal](asio::cancellation_type type) { signal->emit(type); });
  auto invoke = [&]() -> asio::awaitable<pb::ClientCompatResponse> {
    // A signal is not sticky: cancellation during DNS/connect precedes the
    // child slot. Carry the parent's observed cancellation into the child.
    if (parent.cancelled() != asio::cancellation_type::none) {
      RecordError(
          {Code::kCanceled, "call canceled during connection setup", {}},
          *result.mutable_response());
      co_return result;
    }
    if (request.stream_type() == pb::STREAM_TYPE_UNARY)
      co_return co_await InvokeUnaryClient(request, client);
    if (request.method() == "ServerStream")
      co_return co_await InvokeStreaming<pb::ServerStreamRequest,
                                         pb::ServerStreamResponse>(
          request, client, signal);
    if (request.method() == "ClientStream")
      co_return co_await InvokeStreaming<pb::ClientStreamRequest,
                                         pb::ClientStreamResponse>(
          request, client, signal);
    if (request.method() == "BidiStream")
      co_return co_await InvokeStreaming<pb::BidiStreamRequest,
                                         pb::BidiStreamResponse>(
          request, client, signal);
    result.mutable_error()->set_message("unsupported streaming method");
    co_return result;
  };
  auto [exception, response] = co_await asio::co_spawn(
      executor, invoke(),
      asio::bind_cancellation_slot(signal->slot(),
                                   asio::as_tuple(asio::use_awaitable)));
  if (parent.slot().is_connected()) parent.slot().clear();
  co_await asio::this_coro::reset_cancellation_state(
      asio::disable_cancellation());
  co_await transport->Cancel();
  co_await joined.async_receive(asio::as_tuple(asio::use_awaitable));
  if (exception) {
    result.mutable_error()->set_message("unexpected streaming client failure");
    co_return result;
  }
  co_return response;
}
}  // namespace
asio::awaitable<pb::ClientCompatResponse> RunStreamingClient(
    pb::ClientCompatRequest request) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = co_await asio::this_coro::executor;
  pb::ClientCompatResponse result;
  result.set_test_name(request.test_name());
  if ((request.protocol() != pb::PROTOCOL_CONNECT &&
       request.protocol() != pb::PROTOCOL_GRPC &&
       request.protocol() != pb::PROTOCOL_GRPC_WEB) ||
      (request.http_version() != pb::HTTP_VERSION_2 &&
       request.http_version() != pb::HTTP_VERSION_1) ||
      request.has_raw_request()) {
    result.mutable_error()->set_message(
        "unsupported streaming client capability");
    co_return result;
  }
  asio::ssl::context tls(asio::ssl::context::tls_client);
  bool secure = !request.server_tls_cert().empty();
  if (secure) {
    TlsOptions options;
    options.trust_pem = request.server_tls_cert();
    options.certificate_pem = request.client_tls_creds().cert();
    options.private_key_pem = request.client_tls_creds().key();
    Status status = ConfigureTlsContext(tls, false, options);
    if (!status.ok()) {
      result.mutable_error()->set_message(status.message);
      co_return result;
    }
  }
  asio::ip::tcp::resolver resolver(executor);
  auto [resolve_error, endpoints] = co_await resolver.async_resolve(
      request.host(), std::to_string(request.port()),
      asio::as_tuple(asio::use_awaitable));
  if (resolve_error) {
    bool canceled =
        (co_await asio::this_coro::cancellation_state).cancelled() !=
        asio::cancellation_type::none;
    RecordError({canceled ? Code::kCanceled : Code::kUnavailable,
                 resolve_error.message(),
                 {}},
                *result.mutable_response());
    co_return result;
  }
  asio::ip::tcp::socket socket(executor);
  auto [connect_error, endpoint] = co_await asio::async_connect(
      socket, endpoints, asio::as_tuple(asio::use_awaitable));
  (void)endpoint;
  if (connect_error) {
    bool canceled =
        (co_await asio::this_coro::cancellation_state).cancelled() !=
        asio::cancellation_type::none;
    RecordError({canceled ? Code::kCanceled : Code::kUnavailable,
                 connect_error.message(),
                 {}},
                *result.mutable_response());
    co_return result;
  }
  if (request.http_version() == pb::HTTP_VERSION_1) {
    auto connection =
        secure ? Http1Connection::CreateTls(
                     asio::ssl::stream<asio::ip::tcp::socket>(std::move(socket),
                                                              tls),
                     Http1Role::kClient, request.host())
               : Http1Connection::Create(std::move(socket), Http1Role::kClient);
    if (!connection.ok()) {
      RecordError(connection.status(), *result.mutable_response());
      co_return result;
    }
    co_return co_await InvokeOnConnection(request, *connection.value(), secure);
  }
  auto connection = secure
                        ? Http2Connection::CreateTls(
                              asio::ssl::stream<asio::ip::tcp::socket>(
                                  std::move(socket), tls),
                              Http2Session::Role::kClient, request.host())
                        : Http2Connection::Create(std::move(socket),
                                                  Http2Session::Role::kClient);
  if (!connection.ok()) {
    RecordError(connection.status(), *result.mutable_response());
    co_return result;
  }
  co_return co_await InvokeOnConnection(request, *connection.value(), secure);
}
}  // namespace connectrpc::conformance_adapter
