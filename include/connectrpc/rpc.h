#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_RPC_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_RPC_H_

#include <asio/awaitable.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "connectrpc/interceptor.h"
#include "connectrpc/message_stream.h"
#include "connectrpc/unary.h"

namespace connectrpc {
template <typename T>
struct Response {
  Status status;
  std::optional<T> message;
  Metadata headers;
  Metadata trailers;
};
template <typename T>
struct Request {
  T message;
  Metadata headers;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
};
// The exchange supplies HTTP transport and adds :scheme/:authority as needed.
// The transport owns request bytes until it completes. Each call retains its
// exchange independently of the Client object's lifetime.
using UnaryExchange = std::function<asio::awaitable<Result<HttpMessage>>(
    HttpMessage, TransportOptions)>;
using StreamExchange =
    std::function<asio::awaitable<Result<std::shared_ptr<ByteStream>>>(
        Metadata, TransportOptions)>;
struct StreamCallOptions {
  Codec codec = Codec::kProtobuf;
  Metadata headers;
  bool compress = false;
  bool accept_gzip = true;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  StreamLimits limits;       // Incoming response limits.
  StreamLimits send_limits;  // Outgoing request limits.
  Protocol protocol = Protocol::kConnect;
  // Base64 request body and text response negotiation; requires kGrpcWeb.
  bool grpc_web_text = false;
};
struct CallContext {
  Metadata headers;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
};
struct StreamServerOptions {
  StreamLimits limits;       // Incoming request limits.
  StreamLimits send_limits;  // Outgoing response limits.
  // Bounds terminal response flushing after the handler has joined.
  std::chrono::milliseconds completion_timeout{5000};
};
class Client {
 public:
  // With no unary exchange, Unary uses the stream exchange and shared byte
  // transport. Supplying both permits a transport-specific unary override.
  explicit Client(UnaryExchange exchange, StreamExchange stream_exchange = {},
                  std::vector<Interceptor> interceptors = {})
      : exchange_(std::move(exchange)),
        stream_exchange_(std::move(stream_exchange)),
        interceptors_(std::move(interceptors)) {}
  template <typename Input, typename Output>
  asio::awaitable<Result<TypedStream<Input, Output>>> ServerStream(
      std::string procedure, Input input, StreamCallOptions options = {}) const;
  template <typename Input, typename Output>
  asio::awaitable<Result<TypedStream<Input, Output>>> ClientStream(
      std::string procedure, StreamCallOptions options = {}) const;
  template <typename Input, typename Output>
  asio::awaitable<Result<TypedStream<Input, Output>>> BidiStream(
      std::string procedure, StreamCallOptions options = {}) const;
  template <typename Input, typename Output>
  asio::awaitable<Response<Output>> Unary(std::string procedure, Input input,
                                          CallOptions options = {},
                                          MethodOptions method = {}) const;

 private:
  static asio::awaitable<UnaryResponse> ExchangeFramedUnary(
      StreamExchange exchange, std::string procedure, std::string payload,
      CallOptions options);
  template <typename Input, typename Output>
  static asio::awaitable<Response<Output>> InvokeUnary(
      UnaryExchange exchange, StreamExchange stream_exchange,
      std::string procedure, Input input, CallOptions options,
      MethodOptions method, std::vector<Interceptor> interceptors);
  template <typename Input, typename Output>
  static asio::awaitable<Result<TypedStream<Input, Output>>> OpenStream(
      StreamExchange exchange, std::string procedure, StreamShape shape,
      StreamCallOptions options, std::optional<Input> input,
      std::vector<Interceptor> interceptors);
  UnaryExchange exchange_;
  StreamExchange stream_exchange_;
  std::vector<Interceptor> interceptors_;
};
// Register methods before serving. Concurrent calls may read the immutable
// routing table. The server and registered services must outlive all calls.
class Server {
 public:
  explicit Server(std::vector<Interceptor> interceptors = {})
      : interceptors_(std::move(interceptors)) {}
  // Preflight registration before serving; does not modify the routing table.
  Status CanRegister(std::string_view procedure) const;
  template <typename Input, typename Output>
  Status RegisterUnary(
      std::string procedure,
      std::function<asio::awaitable<Response<Output>>(Request<Input>)> handler,
      MethodOptions method = {});
  // Handlers own their returned completion; routing sends End and Finish.
  // Streams must not be retained or used after their handler returns.
  template <typename Input, typename Output>
  Status RegisterServerStream(std::string procedure,
                              std::function<asio::awaitable<StreamCompletion>(
                                  Request<Input>, TypedStream<Output, Input>)>
                                  handler);
  template <typename Input, typename Output>
  Status RegisterClientStream(std::string procedure,
                              std::function<asio::awaitable<Response<Output>>(
                                  CallContext, TypedStream<Output, Input>)>
                                  handler);
  template <typename Input, typename Output>
  Status RegisterBidiStream(std::string procedure,
                            std::function<asio::awaitable<StreamCompletion>(
                                CallContext, TypedStream<Output, Input>)>
                                handler);
  // Handles one accepted HTTP stream, including request-header validation,
  // handler completion, terminal response flushing and unread-request discard.
  // Returns transport/encoding status; the handler's RPC status is sent on
  // wire.
  asio::awaitable<Status> HandleStream(std::shared_ptr<ByteStream> transport,
                                       StreamServerOptions options = {}) const;
  // Routes unary and streaming Connect/gRPC RPCs over shared byte transport.
  asio::awaitable<Status> HandleHttpStream(
      std::shared_ptr<ByteStream> transport,
      StreamServerOptions options = {}) const;
  asio::awaitable<HttpMessage> Handle(
      HttpMessage request, size_t max_message_bytes = 4 * 1024 * 1024,
      size_t max_response_bytes = 4 * 1024 * 1024) const;

 private:
  using Handler = std::function<asio::awaitable<UnaryResponse>(UnaryRequest)>;
  using StreamHandler = std::function<asio::awaitable<StreamCompletion>(
      StreamingRequest, std::shared_ptr<ConnectMessageStream>)>;
  struct StreamMethod {
    StreamShape shape;
    StreamHandler handler;
  };
  struct UnaryMethod {
    Handler handler;
    MethodOptions options;
  };
  Status Register(std::string procedure, Handler handler, MethodOptions method);
  Status RegisterStream(std::string procedure, StreamShape shape,
                        StreamHandler handler);
  std::map<std::string, UnaryMethod, std::less<>> handlers_;
  std::map<std::string, StreamMethod, std::less<>> stream_handlers_;
  std::vector<Interceptor> interceptors_;
};

// Implementation details only below here.
template <typename Input, typename Output>
Status Server::RegisterServerStream(
    std::string procedure,
    std::function<asio::awaitable<StreamCompletion>(Request<Input>,
                                                    TypedStream<Output, Input>)>
        handler) {
  if (!handler) return {Code::kInvalidArgument, "missing handler", {}};
  return RegisterStream(
      std::move(procedure), StreamShape::kServerStreaming,
      [handler = std::move(handler)](
          StreamingRequest request,
          std::shared_ptr<ConnectMessageStream> messages)
          -> asio::awaitable<StreamCompletion> {
        TypedStream<Output, Input> stream(std::move(messages), request.codec);
        auto first = co_await stream.Read();
        if (!first.ok()) co_return StreamCompletion{first.status(), {}};
        if (!*first.value())
          co_return StreamCompletion{
              {Code::kInvalidArgument, "missing request message", {}}, {}};
        // Validate EOF before exposing the request to application code. This
        // rejects extra messages and truncated suffixes without side effects.
        auto eof = co_await stream.Read();
        if (!eof.ok()) co_return StreamCompletion{eof.status(), {}};
        Request<Input> typed{std::move(**first.value()),
                             std::move(request.headers), request.deadline};
        co_return co_await handler(std::move(typed), std::move(stream));
      });
}
template <typename Input, typename Output>
Status Server::RegisterClientStream(
    std::string procedure,
    std::function<asio::awaitable<Response<Output>>(CallContext,
                                                    TypedStream<Output, Input>)>
        handler) {
  if (!handler) return {Code::kInvalidArgument, "missing handler", {}};
  return RegisterStream(
      std::move(procedure), StreamShape::kClientStreaming,
      [handler = std::move(handler)](
          StreamingRequest request,
          std::shared_ptr<ConnectMessageStream> messages)
          -> asio::awaitable<StreamCompletion> {
        TypedStream<Output, Input> stream(std::move(messages), request.codec);
        CallContext context{std::move(request.headers), request.deadline};
        auto response = co_await handler(std::move(context), stream);
        const auto cancellation = co_await asio::this_coro::cancellation_state;
        if (cancellation.cancelled() != asio::cancellation_type::none)
          co_return StreamCompletion{{Code::kCanceled, "handler canceled", {}},
                                     std::move(response.trailers)};
        Status sent;
        if (!response.headers.empty())
          sent = co_await stream.SendHeaders(std::move(response.headers));
        if (sent.ok() && response.status.ok()) {
          if (response.message)
            sent = co_await stream.Write(*response.message);
          else
            sent = {Code::kInternal, "handler returned no message", {}};
        }
        if (!sent.ok()) response.status = std::move(sent);
        co_return StreamCompletion{std::move(response.status),
                                   std::move(response.trailers)};
      });
}
template <typename Input, typename Output>
Status Server::RegisterBidiStream(
    std::string procedure,
    std::function<asio::awaitable<StreamCompletion>(CallContext,
                                                    TypedStream<Output, Input>)>
        handler) {
  if (!handler) return {Code::kInvalidArgument, "missing handler", {}};
  return RegisterStream(
      std::move(procedure), StreamShape::kBidiStreaming,
      [handler = std::move(handler)](
          StreamingRequest request,
          std::shared_ptr<ConnectMessageStream> messages)
          -> asio::awaitable<StreamCompletion> {
        CallContext context{std::move(request.headers), request.deadline};
        co_return co_await handler(
            std::move(context),
            TypedStream<Output, Input>(std::move(messages), request.codec));
      });
}
template <typename Input, typename Output>
asio::awaitable<Result<TypedStream<Input, Output>>> Client::ServerStream(
    std::string procedure, Input input, StreamCallOptions options) const {
  return OpenStream<Input, Output>(
      stream_exchange_, std::move(procedure), StreamShape::kServerStreaming,
      std::move(options), std::move(input), interceptors_);
}
template <typename Input, typename Output>
asio::awaitable<Result<TypedStream<Input, Output>>> Client::ClientStream(
    std::string procedure, StreamCallOptions options) const {
  return OpenStream<Input, Output>(
      stream_exchange_, std::move(procedure), StreamShape::kClientStreaming,
      std::move(options), std::nullopt, interceptors_);
}
template <typename Input, typename Output>
asio::awaitable<Result<TypedStream<Input, Output>>> Client::BidiStream(
    std::string procedure, StreamCallOptions options) const {
  return OpenStream<Input, Output>(
      stream_exchange_, std::move(procedure), StreamShape::kBidiStreaming,
      std::move(options), std::nullopt, interceptors_);
}
template <typename Input, typename Output>
asio::awaitable<Result<TypedStream<Input, Output>>> Client::OpenStream(
    StreamExchange exchange, std::string procedure, StreamShape shape,
    StreamCallOptions options, std::optional<Input> input,
    std::vector<Interceptor> interceptors) {
  co_await asio::this_coro::throw_if_cancelled(false);
  using Opened = Result<TypedStream<Input, Output>>;
  std::shared_ptr<internal_interceptor::Call> hooks;
  auto fail = [&](Status status) {
    if (hooks) hooks->Complete(status);
    return Opened(std::move(status));
  };
  if (!exchange)
    co_return Opened(
        Status{Code::kFailedPrecondition, "missing streaming transport", {}});
  const auto before_open = co_await asio::this_coro::cancellation_state;
  if (before_open.cancelled() != asio::cancellation_type::none)
    co_return Opened(Status{Code::kCanceled, "call canceled", {}});
  StreamingRequest request;
  request.procedure = std::move(procedure);
  request.codec = options.codec;
  request.headers = std::move(options.headers);
  request.gzip = options.compress;
  request.accept_gzip = options.accept_gzip;
  request.deadline = options.deadline;
  request.shape = shape;
  request.protocol = options.protocol;
  request.text = options.grpc_web_text;
  request.accept_text = options.grpc_web_text;
  if (!interceptors.empty()) {
    auto created = internal_interceptor::Call::Create(
        interceptors, {request.procedure, shape, options.protocol,
                       options.codec, true, false, options.deadline});
    if (!created.ok()) co_return fail(created.status());
    hooks = std::move(*created.value());
    auto metadata = co_await hooks->Start(std::move(request.headers));
    if (!metadata.ok()) co_return fail(metadata.status());
    request.headers = std::move(*metadata.value());
  }
  auto headers = EncodeStreamingRequest(request);
  if (!headers.ok()) co_return fail(headers.status());
  TransportOptions transport;
  transport.deadline = options.deadline;
  transport.max_metadata_bytes = options.limits.max_metadata_bytes;
  auto bytes = co_await exchange(std::move(*headers.value()), transport);
  if (!bytes.ok()) co_return fail(bytes.status());
  const auto after_open = co_await asio::this_coro::cancellation_state;
  if (after_open.cancelled() != asio::cancellation_type::none) {
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());
    co_await (*bytes.value())->Cancel();
    co_return fail(Status{Code::kCanceled, "call canceled", {}});
  }

  auto messages = ConnectMessageStream::Client(
      std::move(*bytes.value()), request, options.limits, options.send_limits);
  if (!messages.ok()) co_return fail(messages.status());
  (*messages.value())->SetInterceptors(hooks);
  TypedStream<Input, Output> stream(*messages.value(), options.codec);
  if (input) {
    Status sent = co_await stream.Write(*input);
    if (sent.ok()) sent = co_await stream.CloseSend();
    if (!sent.ok()) {
      // A server may complete its response and stop a large request with
      // RST_STREAM(NO_ERROR). Preserve that response for Read/Finish.
      if (sent.code == Code::kFailedPrecondition) {
        auto response_headers = co_await stream.ReadHeaders();
        if (response_headers.ok()) co_return Opened(std::move(stream));
      }
      if (hooks) hooks->Complete(sent);
      co_await stream.Cancel();
      co_return fail(sent);
    }
  }
  co_return Opened(std::move(stream));
}
template <typename Input, typename Output>
asio::awaitable<Response<Output>> Client::Unary(std::string procedure,
                                                Input input,
                                                CallOptions options,
                                                MethodOptions method) const {
  return InvokeUnary<Input, Output>(exchange_, stream_exchange_,
                                    std::move(procedure), std::move(input),
                                    std::move(options), method, interceptors_);
}
template <typename Input, typename Output>
asio::awaitable<Response<Output>> Client::InvokeUnary(
    UnaryExchange exchange, StreamExchange stream_exchange,
    std::string procedure, Input input, CallOptions options,
    MethodOptions method, std::vector<Interceptor> interceptors) {
  co_await asio::this_coro::throw_if_cancelled(false);
  Response<Output> response;
  std::shared_ptr<internal_interceptor::Call> hooks;
  auto finish = [&] {
    if (hooks) {
      hooks->ResponseHeaders(response.headers);
      hooks->ResponseTrailers(response.trailers);
      hooks->Complete(response.status);
    }
    return std::move(response);
  };
  if (!exchange && !stream_exchange) {
    response.status = {Code::kFailedPrecondition, "missing transport", {}};
    co_return finish();
  }
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    response.status = {Code::kCanceled, "call canceled", {}};
    co_return finish();
  }
  if (options.use_get &&
      (options.protocol != Protocol::kConnect || options.grpc_web_text ||
       method.idempotency != Idempotency::kNoSideEffects)) {
    response.status = {Code::kInvalidArgument,
                       "GET requires a side-effect-free Connect unary method",
                       {}};
    co_return finish();
  }
  if (!interceptors.empty()) {
    auto created = internal_interceptor::Call::Create(
        interceptors,
        {procedure, StreamShape::kUnary, options.protocol, options.codec, true,
         options.use_get, options.transport.deadline});
    if (!created.ok()) {
      response.status = created.status();
      co_return finish();
    }
    hooks = std::move(*created.value());
    auto metadata = co_await hooks->Start(std::move(options.headers));
    if (!metadata.ok()) {
      response.status = metadata.status();
      co_return finish();
    }
    options.headers = std::move(*metadata.value());
  }
  auto bytes = options.use_get
                   ? EncodeDeterministicMessage(input, options.codec)
                   : EncodeMessage(input, options.codec);
  if (!bytes.ok()) {
    response.status = bytes.status();
    co_return finish();
  }
  UnaryResponse decoded;
  if (hooks) hooks->Message(true, bytes.value()->size());
  if (options.protocol != Protocol::kConnect) {
    decoded = co_await ExchangeFramedUnary(std::move(stream_exchange),
                                           std::move(procedure),
                                           std::move(*bytes.value()), options);
  } else {
    auto request = EncodeUnaryRequest(
        std::move(procedure), std::move(*bytes.value()), options, method);
    if (!request.ok()) {
      response.status = request.status();
      co_return finish();
    }
    Result<HttpMessage> wire;
    if (exchange) {
      wire = co_await exchange(std::move(*request.value()), options.transport);
    } else {
      auto opened = co_await stream_exchange(
          std::move(request.value()->headers), options.transport);
      if (!opened.ok()) {
        response.status = opened.status();
        co_return finish();
      }
      wire = co_await UnaryByteStream(std::move(*opened.value()),
                                      std::move(request.value()->body),
                                      options.transport);
    }
    if (!wire.ok()) {
      response.status = wire.status();
      co_return finish();
    }
    decoded = DecodeUnaryResponse(std::move(*wire.value()), options.codec,
                                  options.transport.max_body_bytes);
  }
  response.headers = std::move(decoded.headers);
  response.trailers = std::move(decoded.trailers);
  response.status = std::move(decoded.status);
  if (response.status.ok()) {
    if (hooks) hooks->Message(false, decoded.payload.size());
    Output output;
    response.status = DecodeMessage(decoded.payload, options.codec, output);
    if (response.status.ok())
      response.message = std::move(output);
    else
      response.status.code = Code::kInternal;
  }
  co_return finish();
}
template <typename Input, typename Output>
Status Server::RegisterUnary(
    std::string procedure,
    std::function<asio::awaitable<Response<Output>>(Request<Input>)> handler,
    MethodOptions method) {
  if (!handler) return {Code::kInvalidArgument, "missing handler", {}};
  return Register(
      std::move(procedure),
      [handler = std::move(handler)](
          UnaryRequest request) -> asio::awaitable<UnaryResponse> {
        UnaryResponse result;
        Request<Input> typed;
        typed.headers = std::move(request.headers);
        typed.deadline = request.deadline;
        result.status =
            DecodeMessage(request.payload, request.codec, typed.message);
        if (!result.status.ok()) co_return result;
        Response<Output> response = co_await handler(std::move(typed));
        result.status = std::move(response.status);
        result.headers = std::move(response.headers);
        result.trailers = std::move(response.trailers);
        if (result.status.ok()) {
          if (!response.message) {
            result.status = {
                Code::kInternal, "handler returned no message", {}};
          } else {
            auto bytes = EncodeMessage(*response.message, request.codec);
            if (bytes.ok())
              result.payload = std::move(*bytes.value());
            else
              result.status = bytes.status();
          }
        }
        co_return result;
      },
      method);
}
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_RPC_H_
