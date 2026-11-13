#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_MESSAGE_STREAM_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_MESSAGE_STREAM_H_

#include <asio/awaitable.hpp>
#include <asio/this_coro.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "connectrpc/byte_stream.h"
#include "connectrpc/codec.h"
#include "connectrpc/interceptor.h"
#include "connectrpc/streaming_protocol.h"

namespace connectrpc {
// Connect and gRPC messages over an HTTP byte stream. Factories retain the
// transport. Client request headers must already have been sent; Server
// receives a validated request after its HTTP headers have been read.
// Operations retain their state even when the public handle is destroyed before
// they start. One read operation and one write operation may overlap;
// same-direction overlap is rejected. Applications own and run the underlying
// event loop.
class ConnectMessageStream {
 public:
  static Result<std::shared_ptr<ConnectMessageStream>> Client(
      std::shared_ptr<ByteStream> transport, StreamingRequest request,
      StreamLimits limits = {});
  static Result<std::shared_ptr<ConnectMessageStream>> Client(
      std::shared_ptr<ByteStream> transport, StreamingRequest request,
      StreamLimits receive_limits, StreamLimits send_limits);
  static Result<std::shared_ptr<ConnectMessageStream>> Server(
      std::shared_ptr<ByteStream> transport, StreamingRequest request,
      StreamingResponse response, StreamLimits limits = {});
  static Result<std::shared_ptr<ConnectMessageStream>> Server(
      std::shared_ptr<ByteStream> transport, StreamingRequest request,
      StreamingResponse response, StreamLimits receive_limits,
      StreamLimits send_limits);
  ~ConnectMessageStream();
  ConnectMessageStream(const ConnectMessageStream&) = delete;
  ConnectMessageStream& operator=(const ConnectMessageStream&) = delete;
  asio::awaitable<Result<Metadata>> ReadHeaders();
  // Nullopt is EOF, not RPC success. Inspect Finish for the final RPC status.
  asio::awaitable<Result<std::optional<std::string>>> Read();
  asio::awaitable<Status> Write(std::string payload);
  asio::awaitable<Status>
  SendHeaders();  // Server; otherwise lazy on first write.
  // Replaces response metadata and sends it. Fails after headers were sent.
  asio::awaitable<Status> SendHeaders(Metadata headers);
  asio::awaitable<Status> CloseSend();  // Client request half-close.
  asio::awaitable<Status> End(StreamCompletion completion);  // Server terminal.
  // Requires Read EOF on clients, stops unfinished request writes and returns
  // the received RPC status. On servers, requires End, flushes the response,
  // then discards any unread request without invalidating the response.
  asio::awaitable<StreamCompletion> Finish();
  asio::awaitable<Status> Cancel();
  // Records a protobuf/custom decoder failure. Servers can still send End;
  // clients cancel the malformed response and report an internal error.
  asio::awaitable<Status> FailRead(Status status);

 private:
  friend class Client;
  friend class Server;
  // Attach before any operations start; owned by this call's message engine.
  void SetInterceptors(std::shared_ptr<internal_interceptor::Call> hooks);
  void SetRequestHeaders(Metadata headers);
  class Impl;
  explicit ConnectMessageStream(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};
// Typed message access shared by generated clients and service handlers.
// Send/Receive are Google Protobuf message types. The codec must match the
// validated stream headers. Encoding happens before asynchronous write starts.
template <typename Send, typename Receive>
class TypedStream {
 public:
  TypedStream(std::shared_ptr<ConnectMessageStream> stream, Codec codec)
      : stream_(std::move(stream)), codec_(codec) {}
  TypedStream(const TypedStream&) = default;
  TypedStream& operator=(const TypedStream&) = default;
  TypedStream(TypedStream&&) = default;
  TypedStream& operator=(TypedStream&&) = default;
  asio::awaitable<Result<std::optional<Receive>>> Read() {
    return ReadMessage(stream_, codec_);
  }
  asio::awaitable<Status> Write(const Send& message) {
    return WriteMessage(stream_, EncodeMessage(message, codec_));
  }
  asio::awaitable<Result<Metadata>> ReadHeaders() {
    return stream_->ReadHeaders();
  }
  asio::awaitable<Status> SendHeaders() { return stream_->SendHeaders(); }
  asio::awaitable<Status> SendHeaders(Metadata headers) {
    return stream_->SendHeaders(std::move(headers));
  }
  asio::awaitable<Status> CloseSend() { return stream_->CloseSend(); }
  asio::awaitable<Status> End(StreamCompletion completion) {
    return stream_->End(std::move(completion));
  }
  asio::awaitable<StreamCompletion> Finish() { return stream_->Finish(); }
  asio::awaitable<Status> Cancel() { return stream_->Cancel(); }

 private:
  static asio::awaitable<Result<std::optional<Receive>>> ReadMessage(
      std::shared_ptr<ConnectMessageStream> stream, Codec codec);
  static asio::awaitable<Status> WriteMessage(
      std::shared_ptr<ConnectMessageStream> stream, Result<std::string> bytes);
  std::shared_ptr<ConnectMessageStream> stream_;
  Codec codec_;
};
// Implementation details only below here.
template <typename Send, typename Receive>
asio::awaitable<Result<std::optional<Receive>>>
TypedStream<Send, Receive>::ReadMessage(
    std::shared_ptr<ConnectMessageStream> stream, Codec codec) {
  using Next = Result<std::optional<Receive>>;
  co_await asio::this_coro::throw_if_cancelled(false);
  auto bytes = co_await stream->Read();
  if (!bytes.ok()) co_return Next(bytes.status());
  if (!*bytes.value()) co_return Next(std::nullopt);
  Receive message;
  Status status = DecodeMessage(**bytes.value(), codec, message);
  if (!status.ok()) {
    co_return Next(co_await stream->FailRead(std::move(status)));
  }
  co_return Next(std::move(message));
}
template <typename Send, typename Receive>
asio::awaitable<Status> TypedStream<Send, Receive>::WriteMessage(
    std::shared_ptr<ConnectMessageStream> stream, Result<std::string> bytes) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!bytes.ok()) co_return bytes.status();
  co_return co_await stream->Write(std::move(*bytes.value()));
}
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_MESSAGE_STREAM_H_
