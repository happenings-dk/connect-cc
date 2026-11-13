#include "connectrpc/rpc.h"

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <memory>
#include <utility>

#include "absl/strings/escaping.h"
#include "connectrpc/grpc_protocol.h"
#include "connectrpc/grpc_web_protocol.h"
#include "get_internal.h"

namespace connectrpc {
namespace {
asio::awaitable<Status> WriteFramedRequest(
    std::shared_ptr<ConnectMessageStream> stream, std::string payload) {
  co_await asio::this_coro::throw_if_cancelled(false);
  Status sent = co_await stream->Write(std::move(payload));
  if (sent.ok()) sent = co_await stream->CloseSend();
  if (!sent.ok() && sent.code != Code::kFailedPrecondition)
    co_await stream->Cancel();
  co_return sent;
}
asio::awaitable<UnaryResponse> ReadFramedResponse(
    std::shared_ptr<ConnectMessageStream> stream) {
  UnaryResponse response;
  auto headers = co_await stream->ReadHeaders();
  if (!headers.ok()) {
    response.status = headers.status();
    co_return response;
  }
  response.headers = std::move(*headers.value());
  auto first = co_await stream->Read();
  if (!first.ok()) {
    response.status = first.status();
    co_return response;
  }
  if (*first.value()) {
    response.payload = std::move(**first.value());
    auto eof = co_await stream->Read();
    if (!eof.ok()) {
      response.status = eof.status();
      co_return response;
    }
  }
  auto complete = co_await stream->Finish();
  response.status = std::move(complete.status);
  response.trailers = std::move(complete.trailers);
  co_return response;
}
asio::awaitable<UnaryResponse> ExchangeFramedBody(
    std::shared_ptr<ConnectMessageStream> stream, std::string payload) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = co_await asio::this_coro::executor;
  struct State {
    explicit State(asio::any_io_executor executor) : joined(executor, 1) {}
    asio::experimental::channel<void(asio::error_code)> joined;
    Status written;
  };
  auto state = std::make_shared<State>(executor);
  asio::co_spawn(executor, WriteFramedRequest(stream, std::move(payload)),
                 [state](auto exception, Status written) {
                   state->written =
                       exception
                           ? Status{Code::kInternal, "unary writer failed", {}}
                           : std::move(written);
                   state->joined.try_send(asio::error_code{});
                 });
  auto response = co_await ReadFramedResponse(stream);
  bool canceled = (co_await asio::this_coro::cancellation_state).cancelled() !=
                  asio::cancellation_type::none;
  co_await asio::this_coro::reset_cancellation_state(
      asio::disable_cancellation());
  co_await stream->Cancel();
  co_await state->joined.async_receive(asio::as_tuple(asio::use_awaitable));
  if (canceled)
    response.status = {Code::kCanceled, "call canceled", {}};
  else if (response.status.code == Code::kCanceled && !state->written.ok())
    response.status = state->written;
  if (!response.status.ok()) response.payload.clear();
  co_return response;
}
// The caller runs this operation on a strand, serializing its timer, parent
// cancellation, and handler completion. No handler survives this operation.
template <typename ResponseType = UnaryResponse, typename Handler,
          typename RequestType>
asio::awaitable<ResponseType> InvokeHandler(
    Handler handler, RequestType request,
    std::shared_ptr<ByteStream> transport = {}) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = co_await asio::this_coro::executor;
  auto parent = co_await asio::this_coro::cancellation_state;
  auto deadline = request.deadline;
  ResponseType response;
  if (parent.cancelled() != asio::cancellation_type::none) {
    response.status = {Code::kCanceled, "call canceled", {}};
    co_return response;
  }
  if (request.deadline <= std::chrono::steady_clock::now()) {
    response.status = {Code::kDeadlineExceeded, "deadline exceeded", {}};
    co_return response;
  }
  struct State {
    explicit State(asio::any_io_executor executor)
        : observer_joined(executor, 1) {}
    asio::cancellation_signal signal;
    asio::cancellation_signal stop_observer;
    asio::experimental::channel<void(asio::error_code)> observer_joined;
    Status peer_status;
    bool handler_done = false;
    bool expired = false;
  };
  auto state = std::make_shared<State>(executor);
  if (parent.slot().is_connected())
    parent.slot().assign(
        [state](asio::cancellation_type type) { state->signal.emit(type); });
  asio::steady_timer timer(executor);
  if (request.deadline != std::chrono::steady_clock::time_point::max()) {
    timer.expires_at(request.deadline);
    timer.async_wait([state](asio::error_code error) {
      if (error) return;
      state->expired = true;
      state->signal.emit(asio::cancellation_type::all);
    });
  }
  if (transport) {
    asio::co_spawn(
        executor, transport->WaitClosed(),
        asio::bind_cancellation_slot(
            state->stop_observer.slot(),
            [state](auto exception, Status status) {
              if (!state->handler_done) {
                if (exception)
                  status = {Code::kInternal, "transport observer failed", {}};
                if (!status.ok()) {
                  state->peer_status = std::move(status);
                  state->signal.emit(asio::cancellation_type::all);
                }
              }
              state->observer_joined.try_send(asio::error_code{});
            }));
  }
  auto [exception, result] = co_await asio::co_spawn(
      executor, handler(std::move(request)),
      asio::bind_cancellation_slot(state->signal.slot(),
                                   asio::as_tuple(asio::use_awaitable)));
  timer.cancel();
  if (parent.slot().is_connected()) parent.slot().clear();
  state->handler_done = true;
  if (transport) {
    state->stop_observer.emit(asio::cancellation_type::all);
    co_await state->observer_joined.async_receive(asio::bind_cancellation_slot(
        asio::cancellation_slot{}, asio::as_tuple(asio::use_awaitable)));
  }
  if (state->expired || deadline <= std::chrono::steady_clock::now())
    result.status = {Code::kDeadlineExceeded, "deadline exceeded", {}};
  else if (parent.cancelled() != asio::cancellation_type::none)
    result.status = {Code::kCanceled, "call canceled", {}};
  else if (!state->peer_status.ok())
    result.status = state->peer_status;
  else if (exception)
    result.status = {Code::kInternal, "unexpected handler failure", {}};
  co_return result;
}
HttpMessage ErrorResponse(Status status, int http_override = 0) {
  if (http_override == 405 || http_override == 415)
    status.code = Code::kUnknown;
  UnaryResponse response;
  response.status = std::move(status);
  auto encoded = EncodeUnaryResponse(std::move(response), Codec::kJson, false);
  if (!encoded.ok())
    return {0,
            {{":status", "500"}, {"content-type", "application/json"}},
            "{\"code\":\"internal\"}"};
  if (http_override)
    encoded.value()->headers.front().second = std::to_string(http_override);
  return std::move(*encoded.value());
}
asio::awaitable<Status> SendHttpResponse(std::shared_ptr<ByteStream> transport,
                                         HttpMessage response,
                                         Metadata trailers = {}) {
  Status sent = co_await transport->SendResponse(std::move(response),
                                                 std::move(trailers));
  if (sent.ok()) sent = co_await transport->FinishSend();
  if (sent.ok()) sent = co_await transport->DiscardRead();
  if (!sent.ok()) co_await transport->Cancel();
  co_return sent;
}
}  // namespace
asio::awaitable<UnaryResponse> Client::ExchangeFramedUnary(
    StreamExchange exchange, std::string procedure, std::string payload,
    CallOptions options) {
  co_await asio::this_coro::throw_if_cancelled(false);
  UnaryResponse response;
  if (options.protocol != Protocol::kGrpc &&
      options.protocol != Protocol::kGrpcWeb) {
    response.status = {Code::kUnimplemented, "protocol not implemented", {}};
    co_return response;
  }
  if (!exchange) {
    response.status = {
        Code::kFailedPrecondition, "gRPC requires a stream exchange", {}};
    co_return response;
  }
  StreamingRequest request;
  request.protocol = options.protocol;
  request.text = options.grpc_web_text;
  request.accept_text = options.grpc_web_text;
  request.shape = StreamShape::kUnary;
  request.procedure = std::move(procedure);
  request.codec = options.codec;
  request.gzip = options.compress;
  request.headers = std::move(options.headers);
  request.deadline = options.transport.deadline;
  auto headers = EncodeStreamingRequest(request);
  if (!headers.ok()) {
    response.status = headers.status();
    co_return response;
  }
  auto bytes =
      co_await exchange(std::move(*headers.value()), options.transport);
  if (!bytes.ok()) {
    response.status = bytes.status();
    co_return response;
  }
  // Opening the transport is an asynchronous handoff. A cancellation received
  // there is already recorded on this coroutine but will not be replayed to
  // the new reader task's cancellation slot.
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none) {
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());
    co_await (*bytes.value())->Cancel();
    response.status = {Code::kCanceled, "call canceled", {}};
    co_return response;
  }
  StreamLimits limits{options.transport.max_body_bytes,
                      options.transport.max_body_bytes,
                      options.transport.max_metadata_bytes};
  auto stream = ConnectMessageStream::Client(*bytes.value(), request, limits);
  if (!stream.ok()) {
    response.status = stream.status();
    co_await (*bytes.value())->Cancel();
    co_return response;
  }
  auto [exception, result] = co_await asio::co_spawn(
      asio::make_strand(co_await asio::this_coro::executor),
      ExchangeFramedBody(*stream.value(), std::move(payload)),
      asio::as_tuple(asio::use_awaitable));
  if (exception) result.status = {Code::kInternal, "unary exchange failed", {}};
  co_return result;
}
Status Server::CanRegister(std::string_view procedure) const {
  if (procedure.empty() || procedure.front() != '/' ||
      procedure.find_first_of("?#\r\n ") != std::string::npos) {
    return {Code::kInvalidArgument, "invalid procedure path", {}};
  }
  if (handlers_.contains(procedure) || stream_handlers_.contains(procedure)) {
    return {Code::kAlreadyExists, "duplicate procedure", {}};
  }
  return {};
}
Status Server::Register(std::string procedure, Handler handler,
                        MethodOptions method) {
  Status status = CanRegister(procedure);
  if (!status.ok()) return status;
  handlers_.emplace(std::move(procedure),
                    UnaryMethod{std::move(handler), method});
  return {};
}
Status Server::RegisterStream(std::string procedure, StreamShape shape,
                              StreamHandler handler) {
  Status status = CanRegister(procedure);
  if (!status.ok()) return status;
  stream_handlers_.emplace(std::move(procedure),
                           StreamMethod{shape, std::move(handler)});
  return {};
}
asio::awaitable<Status> Server::HandleStream(
    std::shared_ptr<ByteStream> transport, StreamServerOptions options) const {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!transport)
    co_return Status{Code::kInvalidArgument, "missing byte stream", {}};
  if (options.completion_timeout <= std::chrono::milliseconds::zero())
    co_return Status{Code::kInvalidArgument, "invalid completion timeout", {}};
  auto headers = co_await transport->ReadHeaders();
  if (!headers.ok()) co_return headers.status();
  auto decoded = DecodeStreamingRequest(*headers.value());
  if (!decoded.ok()) {
    int http_status = 0;
    bool has_type = false;
    std::optional<Codec> codec;
    Protocol protocol = Protocol::kConnect;
    bool text = false;
    for (const auto& [name, value] : *headers.value()) {
      if (name == ":method" && value != "POST") http_status = 405;
      if (name == "content-type") {
        has_type = true;
        auto parsed = ParseStreamingContentType(value);
        if (!parsed.ok()) {
          parsed = ParseGrpcContentType(value);
          if (parsed.ok()) protocol = Protocol::kGrpc;
        }
        if (!parsed.ok()) {
          auto web = ParseGrpcWebContentType(value);
          if (web.ok()) {
            parsed = Result<Codec>(web.value()->codec);
            protocol = Protocol::kGrpcWeb;
            text = web.value()->text;
          }
        }
        if (!parsed.ok())
          http_status = 415;

        else
          codec = *parsed.value();
      }
    }
    if (!has_type && http_status == 0) http_status = 415;
    co_await transport->SetDeadline(std::chrono::steady_clock::now() +
                                    options.completion_timeout);
    HttpMessage response;
    Metadata trailers;
    if (http_status != 0 || !codec) {
      // HTTP rejection is not a unary Connect error response. Streaming
      // clients may reject it without consuming its body.
      response.headers = {
          {":status", std::to_string(http_status ? http_status : 415)}};
      if (http_status == 405) response.headers.emplace_back("allow", "POST");
    } else {
      auto encoded_headers =
          EncodeStreamingResponse({*codec, {}, false, protocol, text});
      if (protocol == Protocol::kGrpc) {
        auto encoded_trailers =
            EncodeGrpcTrailers({decoded.status(), {}}, options.send_limits);
        if (!encoded_headers.ok() || !encoded_trailers.ok()) {
          co_await transport->Cancel();
          co_return !encoded_headers.ok() ? encoded_headers.status()
                                          : encoded_trailers.status();
        }
        response.headers = std::move(*encoded_headers.value());
        trailers = std::move(*encoded_trailers.value());
        co_return co_await SendHttpResponse(transport, std::move(response),
                                            std::move(trailers));
      }
      auto terminal =
          protocol == Protocol::kGrpcWeb
              ? EncodeGrpcWebTrailers({decoded.status(), {}},
                                      options.send_limits)
              : EncodeEndStream({decoded.status(), {}}, options.send_limits);
      if (!encoded_headers.ok() || !terminal.ok()) {
        co_await transport->Cancel();
        co_return !encoded_headers.ok() ? encoded_headers.status()
                                        : terminal.status();
      }
      response.headers = std::move(*encoded_headers.value());
      response.body = text ? absl::Base64Escape(*terminal.value())
                           : std::move(*terminal.value());
    }
    co_return co_await SendHttpResponse(transport, std::move(response));
  }
  StreamingRequest request = std::move(*decoded.value());
  auto method = stream_handlers_.find(request.procedure);
  StreamHandler handler;
  if (method != stream_handlers_.end()) {
    request.shape = method->second.shape;
    handler = method->second.handler;
  } else if (request.protocol != Protocol::kConnect) {
    auto unary = handlers_.find(request.procedure);
    if (unary != handlers_.end()) {
      request.shape = StreamShape::kUnary;
      handler = [unary_handler = unary->second.handler](
                    StreamingRequest call,
                    std::shared_ptr<ConnectMessageStream> stream)
          -> asio::awaitable<StreamCompletion> {
        auto first = co_await stream->Read();
        if (!first.ok()) co_return StreamCompletion{first.status(), {}};
        if (!*first.value())
          co_return StreamCompletion{
              {Code::kUnimplemented, "missing request", {}}, {}};
        auto eof = co_await stream->Read();
        if (!eof.ok()) co_return StreamCompletion{eof.status(), {}};
        UnaryRequest request;
        request.procedure = std::move(call.procedure);
        request.codec = call.codec;
        request.payload = std::move(**first.value());
        request.headers = std::move(call.headers);
        request.deadline = call.deadline;
        auto response = co_await unary_handler(std::move(request));
        if ((co_await asio::this_coro::cancellation_state).cancelled() !=
            asio::cancellation_type::none)
          co_return StreamCompletion{{Code::kCanceled, "handler canceled", {}},
                                     {}};
        Status sent;
        if (!response.headers.empty())
          sent = co_await stream->SendHeaders(std::move(response.headers));
        if (sent.ok() && response.status.ok())
          sent = co_await stream->Write(std::move(response.payload));
        if (!sent.ok()) response.status = std::move(sent);
        co_return StreamCompletion{std::move(response.status),
                                   std::move(response.trailers)};
      };
    }
  }
  StreamingResponse response;
  // The wire does not identify RPC cardinality. An unknown route has no
  // registered shape; the decoder's default must not turn it into a bidi RPC.
  if (!handler) request.shape = StreamShape::kServerStreaming;
  if (transport->http_version() == HttpVersion::kHttp1 &&
      (request.protocol == Protocol::kGrpc ||
       request.shape == StreamShape::kBidiStreaming)) {
    Status deadline = co_await transport->SetDeadline(
        std::chrono::steady_clock::now() + options.completion_timeout);
    if (!deadline.ok()) co_return deadline;
    HttpMessage rejection;
    rejection.headers = {{":status", "505"}};
    co_return co_await SendHttpResponse(transport, std::move(rejection));
  }
  response.codec = request.codec;
  response.gzip = request.accept_gzip;
  response.protocol = request.protocol;
  response.text = request.accept_text;
  std::shared_ptr<internal_interceptor::Call> hooks;
  if (handler && !interceptors_.empty()) {
    auto intercepted = internal_interceptor::Call::Create(
        interceptors_, {request.procedure, request.shape, request.protocol,
                        request.codec, false, false, request.deadline});
    if (!intercepted.ok()) {
      co_await transport->Cancel();
      co_return intercepted.status();
    }
    hooks = std::move(*intercepted.value());
  }
  auto created = ConnectMessageStream::Server(
      transport, request, response, options.limits, options.send_limits);
  if (!created.ok()) {
    co_await transport->Cancel();
    co_return created.status();
  }
  auto messages = *created.value();
  messages->SetInterceptors(hooks);
  StreamCompletion completion;
  if (!handler) {
    completion.status = {
        Code::kUnimplemented, "unknown streaming procedure", {}};
  } else {
    auto invoke =
        [handler = std::move(handler), messages,
         hooks](StreamingRequest call) -> asio::awaitable<StreamCompletion> {
      if (hooks) {
        auto headers = co_await hooks->Start(std::move(call.headers));
        if (!headers.ok()) co_return StreamCompletion{headers.status(), {}};
        call.headers = std::move(*headers.value());
        messages->SetRequestHeaders(call.headers);
      }
      co_return co_await handler(std::move(call), messages);
    };
    auto [exception, result] = co_await asio::co_spawn(
        asio::make_strand(co_await asio::this_coro::executor),
        InvokeHandler<StreamCompletion>(std::move(invoke), std::move(request),
                                        transport),
        asio::as_tuple(asio::use_awaitable));
    completion = std::move(result);
    if (exception)
      completion.status = {Code::kInternal, "unexpected handler failure", {}};
  }
  // The RPC handler is joined before cleanup. Allow a bounded interval to send
  // its terminal status, including a deadline exceeded result.
  Status sent = co_await transport->SetDeadline(
      std::chrono::steady_clock::now() + options.completion_timeout);
  if (sent.ok()) sent = co_await messages->End(std::move(completion));
  if (sent.ok()) sent = (co_await messages->Finish()).status;
  if (!sent.ok()) {
    if (hooks) hooks->Complete(sent);
    co_await transport->Cancel();
  }
  co_return sent;
}
asio::awaitable<Status> Server::HandleHttpStream(
    std::shared_ptr<ByteStream> transport, StreamServerOptions options) const {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!transport ||
      options.completion_timeout <= std::chrono::milliseconds::zero())
    co_return Status{Code::kInvalidArgument, "invalid HTTP stream options", {}};
  auto headers = co_await transport->ReadHeaders();
  if (!headers.ok()) co_return headers.status();
  bool get = false;
  for (const auto& [name, value] : *headers.value())
    if (name == ":method" && value == "GET") get = true;
  for (const auto& [name, value] : *headers.value()) {
    if (!get &&
        ((name == ":path" && stream_handlers_.contains(value)) ||
         (name == "content-type" && !ParseUnaryContentType(value).ok())))
      co_return co_await HandleStream(std::move(transport), options);
  }
  HttpMessage request;
  request.headers = std::move(*headers.value());
  Result<UnaryRequest> decoded;
  if (get) {
    // Validate the encoded query against the wire limit now. Decompression
    // belongs to Handle below, using the independent decompressed limit.
    auto normalized =
        internal_get::Decode(request, options.limits.max_message_bytes);
    if (!normalized.status.ok()) {
      decoded = Result<UnaryRequest>(normalized.status);
    } else {
      normalized.message.body.clear();
      decoded = DecodeUnaryRequest(std::move(normalized.message),
                                   options.limits.max_message_bytes);
    }
  } else {
    decoded = DecodeUnaryRequest(request, options.limits.max_message_bytes);
  }
  HttpMessage response;
  if (!decoded.ok()) {
    // Preserve unary HTTP overrides (unsupported media/method, for example).
    response =
        co_await Handle(std::move(request), options.limits.max_message_bytes);
  } else {
    struct HttpResult {
      Status status;
      HttpMessage message;
    };
    auto invoke =
        [this, transport, request = std::move(request),
         options](UnaryRequest call) mutable -> asio::awaitable<HttpResult> {
      co_await asio::this_coro::throw_if_cancelled(false);
      for (;;) {
        auto bytes = co_await transport->Read();
        if (!bytes.ok()) co_return HttpResult{bytes.status(), {}};
        if (!*bytes.value()) break;
        if ((**bytes.value()).size() >
            options.limits.max_message_bytes - request.body.size())
          co_return HttpResult{
              {Code::kResourceExhausted, "request exceeds limit", {}}, {}};
        request.body.append(**bytes.value());
      }
      if (call.deadline != std::chrono::steady_clock::time_point::max()) {
        auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                             call.deadline - std::chrono::steady_clock::now())
                             .count();
        if (remaining <= 0)
          co_return HttpResult{
              {Code::kDeadlineExceeded, "deadline exceeded", {}}, {}};
        for (auto& [name, value] : request.headers)
          if (name == "connect-timeout-ms") value = std::to_string(remaining);
      }
      co_return HttpResult{
          {},
          co_await Handle(std::move(request),
                          options.limits.max_decompressed_bytes,
                          options.send_limits.max_message_bytes)};
    };
    auto [exception, result] = co_await asio::co_spawn(
        asio::make_strand(co_await asio::this_coro::executor),
        InvokeHandler<HttpResult>(std::move(invoke),
                                  std::move(*decoded.value()), transport),
        asio::as_tuple(asio::use_awaitable));
    if (exception)
      result.status = {Code::kInternal, "unary stream task failed", {}};
    response = result.status.ok() ? std::move(result.message)
                                  : ErrorResponse(result.status);
  }
  Status deadline = co_await transport->SetDeadline(
      std::chrono::steady_clock::now() + options.completion_timeout);
  if (!deadline.ok()) co_return deadline;
  co_return co_await SendHttpResponse(std::move(transport),
                                      std::move(response));
}
asio::awaitable<HttpMessage> Server::Handle(HttpMessage request,
                                            size_t max_message_bytes,
                                            size_t max_response_bytes) const {
  co_await asio::this_coro::throw_if_cancelled(false);
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none)
    co_return ErrorResponse({Code::kCanceled, "call canceled", {}});
  // HTTP status overrides are distinct from the Connect error-code mapping.
  bool get = false;
  for (const auto& [name, value] : request.headers)
    if (name == ":method" && value == "GET") get = true;
  if (get) {
    auto normalized =
        internal_get::Decode(std::move(request), max_message_bytes);
    if (!normalized.status.ok())
      co_return ErrorResponse(normalized.status, normalized.http_status);
    request = std::move(normalized.message);
  }
  int override_status = 0;
  bool has_type = false;
  for (const auto& [name, value] : request.headers) {
    if (name == ":method" && value != "POST") override_status = 405;
    if (name == "content-type") {
      has_type = true;
      if (!ParseUnaryContentType(value).ok()) override_status = 415;
    }
  }
  if (!has_type && override_status == 0) override_status = 415;
  auto decoded = DecodeUnaryRequest(std::move(request), max_message_bytes);
  if (!decoded.ok()) co_return ErrorResponse(decoded.status(), override_status);
  auto found = handlers_.find(decoded.value()->procedure);
  if (get && ((found != handlers_.end() && found->second.options.idempotency !=
                                               Idempotency::kNoSideEffects) ||
              stream_handlers_.contains(decoded.value()->procedure))) {
    auto rejected =
        ErrorResponse({Code::kUnimplemented,
                       "GET requires a side-effect-free unary method",
                       {}},
                      405);
    rejected.headers.emplace_back("allow", "POST");
    co_return rejected;
  }
  if (found == handlers_.end())
    co_return ErrorResponse({Code::kUnimplemented, "unknown procedure", {}},
                            404);
  Codec codec = decoded.value()->codec;
  bool gzip = decoded.value()->accept_gzip;
  auto deadline = decoded.value()->deadline;
  std::shared_ptr<internal_interceptor::Call> hooks;
  Handler handler = found->second.handler;
  if (!interceptors_.empty()) {
    auto intercepted = internal_interceptor::Call::Create(
        interceptors_, {decoded.value()->procedure, StreamShape::kUnary,
                        Protocol::kConnect, codec, false, get, deadline});
    if (!intercepted.ok()) co_return ErrorResponse(intercepted.status());
    hooks = std::move(*intercepted.value());
    handler = [next = std::move(handler),
               hooks](UnaryRequest call) -> asio::awaitable<UnaryResponse> {
      auto headers = co_await hooks->Start(std::move(call.headers));
      if (!headers.ok()) {
        UnaryResponse rejected;
        rejected.status = headers.status();
        co_return rejected;
      }
      call.headers = std::move(*headers.value());
      hooks->Message(false, call.payload.size());
      co_return co_await next(std::move(call));
    };
  }
  auto [exception, response] = co_await asio::co_spawn(
      asio::make_strand(co_await asio::this_coro::executor),
      InvokeHandler(std::move(handler), std::move(*decoded.value())),
      asio::as_tuple(asio::use_awaitable));
  if (exception)
    response.status = {Code::kInternal, "unexpected handler failure", {}};
  if (deadline <= std::chrono::steady_clock::now()) {
    response.status = {Code::kDeadlineExceeded, "deadline exceeded", {}};
    response.payload.clear();
  }
  if (response.payload.size() > max_response_bytes) {
    response.status = {Code::kResourceExhausted, "response exceeds limit", {}};
    response.payload.clear();
  }
  if (hooks) {
    if (response.status.ok()) hooks->Message(true, response.payload.size());
    hooks->ResponseHeaders(response.headers);
    hooks->ResponseTrailers(response.trailers);
  }
  Status outcome = response.status;
  auto encoded = EncodeUnaryResponse(std::move(response), codec, gzip);
  if (hooks) hooks->Complete(encoded.ok() ? outcome : encoded.status());
  if (!encoded.ok()) co_return ErrorResponse(encoded.status());
  co_return std::move(*encoded.value());
}
}  // namespace connectrpc
