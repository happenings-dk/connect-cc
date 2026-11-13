#include "connectrpc/message_stream.h"

#include <algorithm>
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <type_traits>
#include <utility>

#include "absl/strings/escaping.h"
#include "connectrpc/grpc_protocol.h"
#include "connectrpc/grpc_web_protocol.h"

namespace connectrpc {
namespace {
using Next = Result<std::optional<std::string>>;
Status Canceled() { return {Code::kCanceled, "stream canceled", {}}; }
Status Precondition(std::string message) {
  return {Code::kFailedPrecondition, std::move(message), {}};
}
class BusyGuard {
 public:
  explicit BusyGuard(bool& busy)
      : busy_(busy), acquired_(!std::exchange(busy, true)) {}
  ~BusyGuard() {
    if (acquired_) busy_ = false;
  }
  BusyGuard(const BusyGuard&) = delete;
  BusyGuard& operator=(const BusyGuard&) = delete;
  bool acquired() const { return acquired_; }

 private:
  bool& busy_;
  bool acquired_;
};
template <typename T>
T Failure(Status status) {
  if constexpr (std::is_same_v<T, StreamCompletion>)
    return {std::move(status), {}};
  else
    return T(std::move(status));
}
template <typename State, typename T>
asio::awaitable<T> RunOperation(std::shared_ptr<State> state,
                                asio::awaitable<T> operation,
                                bool read_operation) {
  co_await asio::this_coro::throw_if_cancelled(false);
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_await state->OnCancellation(read_operation);
    co_return Failure<T>(Canceled());
  }
  co_return co_await std::move(operation);
}
template <typename State, typename T>
asio::awaitable<T> Dispatch(std::shared_ptr<State> state,
                            asio::awaitable<T> operation,
                            bool honor_cancellation = true,
                            bool read_operation = false) {
  co_await asio::this_coro::throw_if_cancelled(false);
  const auto before_dispatch = co_await asio::this_coro::cancellation_state;
  if (honor_cancellation &&
      before_dispatch.cancelled() != asio::cancellation_type::none) {
    co_await Dispatch(state, state->OnCancellation(read_operation), false);
    co_return Failure<T>(Canceled());
  }
  auto [exception, result] = co_await asio::co_spawn(
      state->executor(),
      RunOperation(state, std::move(operation), read_operation),
      asio::as_tuple(asio::use_awaitable));
  if (exception)
    co_return Failure<T>(
        {Code::kInternal, "unexpected stream coroutine failure", {}});
  const auto after_dispatch = co_await asio::this_coro::cancellation_state;
  if (honor_cancellation &&
      after_dispatch.cancelled() != asio::cancellation_type::none) {
    co_await Dispatch(state, state->OnCancellation(read_operation), false);
    co_return Failure<T>(Canceled());
  }
  co_return std::move(result);
}
}  // namespace
class ConnectMessageStream::Impl {
 public:
  Impl(std::shared_ptr<ByteStream> transport, bool client,
       StreamingRequest request, StreamingResponse response,
       StreamLimits limits, StreamLimits send_limits)
      : transport_(std::move(transport)),
        executor_(asio::make_strand(transport_->executor())),
        client_(client),
        request_(std::move(request)),
        response_(std::move(response)),
        limits_(limits),
        send_limits_(send_limits) {
    if (!client_)
      decoder_ = std::make_unique<StreamingDecoder>(false, request_.gzip,
                                                    limits_, request_.protocol);
  }
  asio::any_io_executor executor() const { return executor_; }
  ~Impl() {
    if (client_ && hooks_) {
      if (eof_) hooks_->ResponseTrailers(completion_.trailers);
      hooks_->Complete(eof_           ? completion_.status
                       : !error_.ok() ? error_
                                      : Canceled());
    }
  }
  void SetInterceptors(std::shared_ptr<internal_interceptor::Call> hooks) {
    hooks_ = std::move(hooks);
  }
  void SetRequestHeaders(Metadata headers) {
    request_.headers = std::move(headers);
  }
  asio::awaitable<Result<Metadata>> ReadHeaders();
  asio::awaitable<Next> Read();
  asio::awaitable<Status> Write(std::string payload);
  asio::awaitable<Status> SendHeaders();
  asio::awaitable<Status> SendHeaders(Metadata headers);
  asio::awaitable<Status> CloseSend();
  asio::awaitable<Status> End(StreamCompletion completion);
  asio::awaitable<StreamCompletion> Finish();
  asio::awaitable<Status> Cancel();
  asio::awaitable<Status> FailRead(Status status);
  asio::awaitable<Status> OnCancellation(bool read_operation);

 private:
  asio::awaitable<Next> ReadBody();
  GrpcWebTextDecoder text_decoder_;
  asio::awaitable<Status> EnsureReadHeaders();
  asio::awaitable<Status> EnsureSendHeaders();
  std::shared_ptr<ByteStream> transport_;
  asio::any_io_executor executor_;
  bool client_;
  StreamingRequest request_;
  StreamingResponse response_;
  StreamLimits limits_;
  StreamLimits send_limits_;
  std::unique_ptr<StreamingDecoder> decoder_;
  std::string pending_;
  size_t offset_ = 0;
  bool reading_ = false;
  bool writing_ = false;
  bool headers_sent_ = false;
  bool send_closed_ = false;
  bool eof_ = false;
  bool finalized_ = false;
  int read_messages_ =
      0;  // Saturates at two: only single-message shapes count.
  int written_messages_ = 0;
  Status error_;
  Status read_error_;
  Status write_error_;
  StreamCompletion completion_;
  std::optional<StreamCompletion> initial_completion_;
  std::shared_ptr<internal_interceptor::Call> hooks_;
  Status rpc_outcome_;
};
asio::awaitable<Status> ConnectMessageStream::Impl::EnsureReadHeaders() {
  if (!error_.ok()) co_return error_;
  if (decoder_) co_return Status{};
  auto headers = co_await transport_->ReadHeaders();
  if (!headers.ok()) co_return error_ = headers.status();
  auto response =
      request_.protocol == Protocol::kGrpcWeb
          ? DecodeGrpcWebResponse(*headers.value(), request_.codec,
                                  request_.accept_gzip)
      : request_.protocol == Protocol::kGrpc
          ? DecodeGrpcResponse(*headers.value(), request_.codec,
                               request_.accept_gzip)
          : DecodeStreamingResponse(*headers.value(), request_.codec,
                                    request_.accept_gzip);
  if (!response.ok()) {
    error_ = response.status();
    co_await transport_->Cancel();
    co_return error_;
  }
  response_ = std::move(*response.value());
  if (hooks_) hooks_->ResponseHeaders(response_.headers);
  if (request_.protocol != Protocol::kConnect &&
      std::any_of(
          headers.value()->begin(), headers.value()->end(),
          [](const auto& field) { return field.first == "grpc-status"; })) {
    // A misplaced status in initial headers cannot override later data or
    // trailers. Peek one chunk and retain it for the message reader.
    auto first = co_await ReadBody();
    if (!first.ok()) {
      co_return co_await FailRead(first.status());
    }
    if (*first.value()) {
      pending_ = std::move(**first.value());
    } else {
      auto trailers = co_await transport_->ReadTrailers();
      if (!trailers.ok()) co_return error_ = trailers.status();
      if (trailers.value()->empty()) {
        auto completion = DecodeGrpcTrailers(*headers.value(), limits_);
        if (!completion.ok()) {
          error_ = completion.status();
          co_await transport_->Cancel();
          co_return error_;
        }
        initial_completion_ = std::move(*completion.value());
        // Trailers-only custom fields are trailing metadata. Keep the wire
        // header view too: gateways can combine leading and trailing fields
        // into one block, with no way to recover their original distinction.
      }
    }
  }
  decoder_ = std::make_unique<StreamingDecoder>(true, response_.gzip, limits_,
                                                request_.protocol);
  co_return Status{};
}
asio::awaitable<Next> ConnectMessageStream::Impl::ReadBody() {
  bool text = request_.protocol == Protocol::kGrpcWeb &&
              (client_ ? response_.text : request_.text);
  for (;;) {
    auto bytes = co_await transport_->Read();
    if (!bytes.ok() || !text) co_return bytes;
    if (!*bytes.value()) {
      Status finished = text_decoder_.Finish();
      if (!finished.ok()) co_return Next(finished);
      co_return bytes;
    }
    auto decoded = text_decoder_.Read(**bytes.value());
    if (!decoded.ok()) co_return Next(decoded.status());
    if (!decoded.value()->empty()) co_return Next(std::move(*decoded.value()));
  }
}
asio::awaitable<Status> ConnectMessageStream::Impl::EnsureSendHeaders() {
  if (!error_.ok()) co_return error_;
  if (client_ || headers_sent_) co_return Status{};
  if (hooks_) hooks_->ResponseHeaders(response_.headers);
  auto headers = EncodeStreamingResponse(response_);
  if (!headers.ok()) co_return headers.status();
  Status sent = co_await transport_->SendHeaders(std::move(*headers.value()));
  if (!sent.ok()) co_return error_ = sent;
  headers_sent_ = true;
  co_return Status{};
}
asio::awaitable<Result<Metadata>> ConnectMessageStream::Impl::ReadHeaders() {
  BusyGuard guard(reading_);
  if (!guard.acquired())
    co_return Result<Metadata>(Precondition("concurrent stream reads"));
  Status ready = co_await EnsureReadHeaders();
  if (!ready.ok()) co_return Result<Metadata>(ready);
  co_return Result<Metadata>(client_ ? response_.headers : request_.headers);
}
asio::awaitable<Next> ConnectMessageStream::Impl::Read() {
  BusyGuard guard(reading_);
  if (!guard.acquired())
    co_return Next(Precondition("concurrent stream reads"));
  if (!read_error_.ok()) co_return Next(read_error_);
  Status ready = co_await EnsureReadHeaders();
  if (!ready.ok()) co_return Next(ready);
  if (eof_) co_return Next(std::nullopt);
  for (;;) {
    if (!error_.ok()) co_return Next(error_);
    if (offset_ < pending_.size()) {
      std::string_view bytes(pending_.data() + offset_,
                             pending_.size() - offset_);
      auto message = decoder_->ReadOne(bytes);
      offset_ = pending_.size() - bytes.size();
      if (!message.ok()) {
        co_return Next(co_await FailRead(message.status()));
      }
      if (*message.value()) {
        read_messages_ = std::min(2, read_messages_ + 1);
        bool single =
            request_.shape == StreamShape::kUnary ||
            (client_ ? request_.shape == StreamShape::kClientStreaming
                     : request_.shape == StreamShape::kServerStreaming);
        if (single && read_messages_ > 1)
          co_return Next(co_await FailRead(
              {Code::kUnimplemented, "expected exactly one message", {}}));
        if (hooks_) hooks_->Message(false, (**message.value()).size());
        co_return message;
      }
      continue;
    }
    pending_.clear();
    offset_ = 0;
    auto bytes = co_await ReadBody();
    if (!bytes.ok()) {
      if ((!client_ && bytes.status().code == Code::kCanceled) ||
          bytes.status().code == Code::kInvalidArgument ||
          bytes.status().code == Code::kDataLoss)
        co_return Next(co_await FailRead(bytes.status()));
      co_return Next(error_ = bytes.status());
    }
    if (!*bytes.value()) {
      auto finished = client_ && initial_completion_ &&
                              request_.protocol == Protocol::kGrpcWeb
                          ? Result<StreamCompletion>(*initial_completion_)
                          : decoder_->Finish();
      if (!finished.ok()) {
        co_return Next(co_await FailRead(finished.status()));
      }
      if (client_ && request_.protocol == Protocol::kGrpc) {
        auto trailers = co_await transport_->ReadTrailers();
        if (!trailers.ok())
          co_return Next(co_await FailRead(trailers.status()));
        if (initial_completion_ && trailers.value()->empty()) {
          if (read_messages_ != 0)
            co_return Next(co_await FailRead(
                {Code::kInternal, "message after trailers-only response", {}}));
          finished = Result<StreamCompletion>(*initial_completion_);
        } else {
          finished = DecodeGrpcTrailers(*trailers.value(), limits_);
        }
        if (!finished.ok())
          co_return Next(co_await FailRead(finished.status()));
      }
      bool single = request_.shape == StreamShape::kUnary ||
                    (client_ ? request_.shape == StreamShape::kClientStreaming
                             : request_.shape == StreamShape::kServerStreaming);
      if (single && read_messages_ != 1 && finished.value()->status.ok())
        co_return Next(co_await FailRead(
            {Code::kUnimplemented, "expected exactly one message", {}}));
      completion_ = std::move(*finished.value());
      eof_ = true;
      co_return Next(std::nullopt);
    }
    pending_ = std::move(**bytes.value());
  }
}
asio::awaitable<Status> ConnectMessageStream::Impl::Write(std::string payload) {
  BusyGuard guard(writing_);
  if (!guard.acquired()) co_return Precondition("concurrent stream writes");
  if (finalized_) co_return Precondition("RPC finished");
  if (!error_.ok()) co_return error_;
  if (!read_error_.ok()) co_return read_error_;
  if (!write_error_.ok()) co_return write_error_;
  if (send_closed_) co_return Precondition("send direction closed");
  bool single = request_.shape == StreamShape::kUnary ||
                (client_ ? request_.shape == StreamShape::kServerStreaming
                         : request_.shape == StreamShape::kClientStreaming);
  if (single && written_messages_ != 0)
    co_return write_error_ =
        Status{client_ ? Code::kFailedPrecondition : Code::kInternal,
               "expected exactly one message",
               {}};
  size_t message_bytes = payload.size();
  auto frame = EncodeStreamingMessage(std::move(payload),
                                      client_ ? request_.gzip : response_.gzip,
                                      send_limits_);
  if (!frame.ok()) co_return frame.status();
  if (hooks_) hooks_->Message(true, message_bytes);
  Status ready = co_await EnsureSendHeaders();
  if (!ready.ok()) co_return ready;
  if (request_.protocol == Protocol::kGrpcWeb &&
      (client_ ? request_.text : response_.text))
    *frame.value() = absl::Base64Escape(*frame.value());
  Status sent = co_await transport_->Write(std::move(*frame.value()));
  if (!sent.ok())
    write_error_ = sent;
  else
    written_messages_ = std::min(2, written_messages_ + 1);
  co_return sent;
}
asio::awaitable<Status> ConnectMessageStream::Impl::SendHeaders() {
  BusyGuard guard(writing_);
  if (!guard.acquired()) co_return Precondition("concurrent stream writes");
  if (client_) co_return Precondition("client headers already sent");
  co_return co_await EnsureSendHeaders();
}
asio::awaitable<Status> ConnectMessageStream::Impl::SendHeaders(
    Metadata headers) {
  BusyGuard guard(writing_);
  if (!guard.acquired()) co_return Precondition("concurrent stream writes");
  if (client_ || headers_sent_) co_return Precondition("headers already sent");
  response_.headers = std::move(headers);
  co_return co_await EnsureSendHeaders();
}
asio::awaitable<Status> ConnectMessageStream::Impl::CloseSend() {
  BusyGuard guard(writing_);
  if (!guard.acquired()) co_return Precondition("concurrent stream writes");
  if (!error_.ok()) co_return error_;
  if (!client_)
    co_return Precondition("server must send terminal status with End");
  if (send_closed_) co_return Status{};
  if ((request_.shape == StreamShape::kServerStreaming ||
       request_.shape == StreamShape::kUnary) &&
      written_messages_ != 1)
    co_return Precondition("server streaming requires one request message");
  Status closed = co_await transport_->CloseSend({});
  if (!closed.ok()) co_return write_error_ = closed;
  send_closed_ = true;
  co_return Status{};
}
asio::awaitable<Status> ConnectMessageStream::Impl::End(
    StreamCompletion completion) {
  BusyGuard guard(writing_);
  if (!guard.acquired()) co_return Precondition("concurrent stream writes");
  if (!error_.ok()) co_return error_;
  if (client_) co_return Precondition("client cannot send terminal status");
  if (send_closed_) co_return Precondition("terminal status already sent");
  if (completion.status.ok() && !read_error_.ok())
    completion.status = read_error_;
  if (completion.status.ok() && !write_error_.ok())
    completion.status = write_error_;
  if (completion.status.ok() &&
      (request_.shape == StreamShape::kClientStreaming ||
       request_.shape == StreamShape::kUnary) &&
      written_messages_ != 1)
    completion.status = {Code::kInternal, "expected exactly one response", {}};
  rpc_outcome_ = completion.status;
  if (hooks_) {
    // An early rejection may not have sent headers yet. Keep hook ordering
    // consistent without committing HTTP headers before terminal encoding.
    hooks_->ResponseHeaders(response_.headers);
    hooks_->ResponseTrailers(completion.trailers);
  }
  if (request_.protocol == Protocol::kGrpc) {
    auto trailers = EncodeGrpcTrailers(completion, send_limits_);
    if (!trailers.ok()) co_return trailers.status();
    Status status = co_await EnsureSendHeaders();
    if (status.ok())
      status = co_await transport_->CloseSend(std::move(*trailers.value()));
    if (!status.ok()) co_return error_ = status;
    send_closed_ = true;
    co_return Status{};
  }
  auto frame = request_.protocol == Protocol::kGrpcWeb
                   ? EncodeGrpcWebTrailers(completion, send_limits_)
                   : EncodeEndStream(completion, send_limits_);
  if (!frame.ok()) co_return frame.status();
  if (request_.protocol == Protocol::kGrpcWeb && response_.text)
    *frame.value() = absl::Base64Escape(*frame.value());
  Status status = co_await EnsureSendHeaders();
  if (status.ok())
    status = co_await transport_->Write(std::move(*frame.value()));
  if (status.ok()) status = co_await transport_->CloseSend({});
  if (!status.ok()) co_return error_ = status;
  send_closed_ = true;
  co_return Status{};
}
asio::awaitable<StreamCompletion> ConnectMessageStream::Impl::Finish() {
  BusyGuard guard(reading_);
  if (!guard.acquired())
    co_return StreamCompletion{Precondition("concurrent stream reads"), {}};
  if (finalized_) co_return completion_;
  if (client_ && eof_) {
    // A fully received response is authoritative even if the server stopped
    // consuming requests. Reset unfinished outgoing data to wake blocked writes
    // rather than waiting forever for request flow-control credit.
    finalized_ = true;
    send_closed_ = true;
    co_await transport_->Cancel();
    if (hooks_) {
      hooks_->ResponseTrailers(completion_.trailers);
      hooks_->Complete(completion_.status);
    }
    co_return completion_;
  }
  if (!error_.ok()) {
    if (client_ && hooks_) hooks_->Complete(error_);
    co_return StreamCompletion{error_, completion_.trailers};
  }
  if (!send_closed_ || client_)
    co_return StreamCompletion{
        Precondition("Finish requires client EOF or server End"), {}};
  Status flushed = co_await transport_->FinishSend();
  if (!flushed.ok()) co_return StreamCompletion{error_ = flushed, {}};
  Status discarded = co_await transport_->DiscardRead();
  if (!discarded.ok()) co_return StreamCompletion{error_ = discarded, {}};
  finalized_ = true;
  pending_.clear();
  if (hooks_) hooks_->Complete(rpc_outcome_);
  co_return completion_;
}
asio::awaitable<Status> ConnectMessageStream::Impl::FailRead(Status status) {
  if (status.ok()) co_return Precondition("FailRead requires an error");
  if (!read_error_.ok()) co_return read_error_;
  if (status.code == Code::kDataLoss) status.code = Code::kInvalidArgument;
  if (client_) {
    if (status.code == Code::kInvalidArgument) status.code = Code::kInternal;
    error_ = status;
    co_await transport_->Cancel();
    if (hooks_) hooks_->Complete(error_);
  }
  read_error_ = std::move(status);
  co_return read_error_;
}
asio::awaitable<Status> ConnectMessageStream::Impl::Cancel() {
  if (finalized_) co_return Status{};
  if (error_.ok()) error_ = Canceled();
  co_await transport_->Cancel();
  if (client_ && hooks_) hooks_->Complete(error_);
  co_return Status{};
}
asio::awaitable<Status> ConnectMessageStream::Impl::OnCancellation(
    bool read_operation) {
  // A canceled server read has not changed the response direction. Keep it
  // available for the router to send a terminal deadline/cancellation result.
  if (read_operation && !client_) co_return co_await FailRead(Canceled());
  co_return co_await Cancel();
}
ConnectMessageStream::ConnectMessageStream(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
ConnectMessageStream::~ConnectMessageStream() = default;
Result<std::shared_ptr<ConnectMessageStream>> ConnectMessageStream::Client(
    std::shared_ptr<ByteStream> transport, StreamingRequest request,
    StreamLimits limits) {
  return Client(std::move(transport), std::move(request), limits, limits);
}
Result<std::shared_ptr<ConnectMessageStream>> ConnectMessageStream::Client(
    std::shared_ptr<ByteStream> transport, StreamingRequest request,
    StreamLimits limits, StreamLimits send_limits) {
  using Created = Result<std::shared_ptr<ConnectMessageStream>>;
  if (!transport) return Created(Precondition("missing byte stream"));
  if (transport->http_version() == HttpVersion::kHttp1 &&
      (request.protocol == Protocol::kGrpc ||
       request.shape == StreamShape::kBidiStreaming))
    return Created(Status{Code::kUnimplemented,
                          "gRPC and bidirectional RPCs require HTTP/2",
                          {}});
  auto valid = EncodeStreamingRequest(request);
  if (!valid.ok()) return Created(valid.status());
  return Created(std::shared_ptr<ConnectMessageStream>(new ConnectMessageStream(
      std::make_shared<Impl>(std::move(transport), true, std::move(request),
                             StreamingResponse{}, limits, send_limits))));
}
Result<std::shared_ptr<ConnectMessageStream>> ConnectMessageStream::Server(
    std::shared_ptr<ByteStream> transport, StreamingRequest request,
    StreamingResponse response, StreamLimits limits) {
  return Server(std::move(transport), std::move(request), std::move(response),
                limits, limits);
}
Result<std::shared_ptr<ConnectMessageStream>> ConnectMessageStream::Server(
    std::shared_ptr<ByteStream> transport, StreamingRequest request,
    StreamingResponse response, StreamLimits limits, StreamLimits send_limits) {
  using Created = Result<std::shared_ptr<ConnectMessageStream>>;
  if (!transport) return Created(Precondition("missing byte stream"));
  if (request.protocol != response.protocol ||
      request.codec != response.codec ||
      (response.gzip && !request.accept_gzip))
    return Created(
        Precondition("response does not match negotiated codec/compression"));
  if (transport->http_version() == HttpVersion::kHttp1 &&
      (request.protocol == Protocol::kGrpc ||
       request.shape == StreamShape::kBidiStreaming))
    return Created(Status{Code::kUnimplemented,
                          "gRPC and bidirectional RPCs require HTTP/2",
                          {}});
  auto valid = EncodeStreamingResponse(response);
  if (!valid.ok()) return Created(valid.status());
  return Created(std::shared_ptr<ConnectMessageStream>(new ConnectMessageStream(
      std::make_shared<Impl>(std::move(transport), false, std::move(request),
                             std::move(response), limits, send_limits))));
}
void ConnectMessageStream::SetInterceptors(
    std::shared_ptr<internal_interceptor::Call> hooks) {
  impl_->SetInterceptors(std::move(hooks));
}
void ConnectMessageStream::SetRequestHeaders(Metadata headers) {
  impl_->SetRequestHeaders(std::move(headers));
}
asio::awaitable<Result<Metadata>> ConnectMessageStream::ReadHeaders() {
  return Dispatch(impl_, impl_->ReadHeaders(), true, true);
}
asio::awaitable<Next> ConnectMessageStream::Read() {
  return Dispatch(impl_, impl_->Read(), true, true);
}
asio::awaitable<Status> ConnectMessageStream::Write(std::string payload) {
  return Dispatch(impl_, impl_->Write(std::move(payload)));
}
asio::awaitable<Status> ConnectMessageStream::SendHeaders() {
  return Dispatch(impl_, impl_->SendHeaders());
}
asio::awaitable<Status> ConnectMessageStream::SendHeaders(Metadata headers) {
  return Dispatch(impl_, impl_->SendHeaders(std::move(headers)));
}
asio::awaitable<Status> ConnectMessageStream::CloseSend() {
  return Dispatch(impl_, impl_->CloseSend());
}
asio::awaitable<Status> ConnectMessageStream::End(StreamCompletion completion) {
  return Dispatch(impl_, impl_->End(std::move(completion)));
}
asio::awaitable<StreamCompletion> ConnectMessageStream::Finish() {
  return Dispatch(impl_, impl_->Finish());
}
asio::awaitable<Status> ConnectMessageStream::FailRead(Status status) {
  return Dispatch(impl_, impl_->FailRead(std::move(status)));
}
asio::awaitable<Status> ConnectMessageStream::Cancel() {
  return Dispatch(impl_, impl_->Cancel(), false);
}
}  // namespace connectrpc
