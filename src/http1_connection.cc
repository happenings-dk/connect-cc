#include "connectrpc/http1_connection.h"

#include <openssl/ssl.h>

#include <algorithm>
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <deque>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "connectrpc/tls.h"

namespace connectrpc {
namespace {
using Notification = asio::experimental::channel<void(asio::error_code)>;
using Bytes = Result<std::optional<std::string>>;
using Handle = Result<std::shared_ptr<Http1ByteStream>>;
using Socket = std::variant<asio::ip::tcp::socket,
                            asio::ssl::stream<asio::ip::tcp::socket>>;
using Time = std::chrono::steady_clock::time_point;
Status Canceled() { return {Code::kCanceled, "operation canceled", {}}; }
Status Closed() {
  return {Code::kUnavailable, "HTTP/1.1 connection closed", {}};
}
Status Busy() {
  return {Code::kFailedPrecondition, "concurrent stream operation", {}};
}
Status IoError(asio::error_code error) {
  return {error == asio::error::operation_aborted ? Code::kCanceled
                                                  : Code::kUnavailable,
          error.message(),
          {}};
}
void Notify(Notification& event) { event.try_send(asio::error_code{}); }
asio::awaitable<bool> Wait(Notification& event) {
  auto [error] =
      co_await event.async_receive(asio::as_tuple(asio::use_awaitable));
  co_return !error;
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
asio::awaitable<T> RunOperation(asio::awaitable<T> operation,
                                bool honor_cancellation) {
  co_await asio::this_coro::throw_if_cancelled(false);
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (honor_cancellation &&
      cancellation.cancelled() != asio::cancellation_type::none)
    co_return T(Canceled());
  co_return co_await std::move(operation);
}
template <typename T, typename Connection>
asio::awaitable<T> Dispatch(std::shared_ptr<Connection> connection,
                            asio::awaitable<T> operation,
                            bool honor_cancellation = true,
                            uint64_t abort_id = 0) {
  co_await asio::this_coro::throw_if_cancelled(false);
  T result;
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (honor_cancellation &&
      cancellation.cancelled() != asio::cancellation_type::none) {
    result = T(Canceled());
  } else {
    auto [exception, value] = co_await asio::co_spawn(
        connection->executor(),
        RunOperation(std::move(operation), honor_cancellation),
        asio::as_tuple(asio::use_awaitable));
    result =
        exception
            ? T(Status{Code::kInternal, "unexpected coroutine failure", {}})
            : std::move(value);
  }
  Status status;
  if constexpr (std::is_same_v<T, Status>)
    status = result;
  else
    status = result.status();
  if (abort_id != 0 && status.code == Code::kCanceled) {
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());
    co_await asio::co_spawn(connection->executor(),
                            connection->CancelStream(abort_id),
                            asio::as_tuple(asio::use_awaitable));
  }
  co_return std::move(result);
}
}  // namespace

class Http1Connection::Impl : public std::enable_shared_from_this<Impl> {
 public:
  Impl(Socket socket, Http1Role role, Http1ConnectionOptions options,
       std::string hostname, Time handshake_deadline, bool tls_ready = false)
      : socket_(std::move(socket)),
        strand_(asio::make_strand(Lowest().get_executor())),
        role_(role),
        options_(options),
        hostname_(std::move(hostname)),
        handshake_deadline_(handshake_deadline),
        tls_ready_(tls_ready),
        reader_event_(strand_, 1),
        writer_event_(strand_, 1),
        accept_event_(strand_, 1),
        stopped_event_(strand_, 1),
        joined_(strand_, 1),
        shutdown_timer_(strand_) {}
  auto executor() const { return strand_; }
  bool client() const { return role_ == Http1Role::kClient; }
  Status Initialize();
  asio::awaitable<Status> Run();
  asio::awaitable<Handle> Open(Metadata headers, Time deadline);
  asio::awaitable<Handle> Accept();
  asio::awaitable<Status> Cancel();
  asio::awaitable<Status> Shutdown(Time deadline);
  asio::awaitable<Result<Metadata>> ReadHeaders(uint64_t id);
  asio::awaitable<Bytes> Read(uint64_t id);
  asio::awaitable<Status> SendHeaders(uint64_t id, Metadata headers);
  asio::awaitable<Status> SendResponse(uint64_t id, HttpMessage response,
                                       Metadata trailers);
  asio::awaitable<Status> Write(uint64_t id, std::string bytes);
  asio::awaitable<Status> CloseSend(uint64_t id, Metadata trailers);
  asio::awaitable<Status> FinishSend(uint64_t id);
  asio::awaitable<Status> DiscardRead(uint64_t id);
  asio::awaitable<Result<Metadata>> Finish(uint64_t id);
  asio::awaitable<Result<Metadata>> ReadTrailers(uint64_t id);
  asio::awaitable<Status> WaitClosed(uint64_t id);
  asio::awaitable<Status> SetDeadline(uint64_t id, Time deadline);
  asio::awaitable<Status> CancelStream(uint64_t id);
  void Release(uint64_t id);

 private:
  struct Call {
    Call(asio::any_io_executor executor, Http1Role role, Http1Limits limits,
         uint64_t generation)
        : encoder(role, limits),
          id(generation),
          read_event(executor, 1),
          write_event(executor, 1),
          close_event(executor, 1),
          timer(executor) {}
    Http1Encoder encoder;
    uint64_t id;
    Metadata headers;
    Metadata trailers;
    std::deque<std::string> incoming;
    size_t incoming_bytes = 0;
    Notification read_event;
    Notification write_event;
    Notification close_event;
    asio::steady_timer timer;
    Time deadline = Time::max();
    std::optional<Status> failure;
    bool got_headers = false;
    bool accepted = false;
    bool ended = false;
    bool keep_alive = true;
    bool released = false;
    bool discarding = false;
    bool reading = false;
    bool writing = false;
    bool observing = false;
    bool peer_eof = false;
    bool send_started = false;
    bool send_closed = false;
    bool send_flushed = false;
  };
  struct Output {
    std::string bytes;
    bool end = false;
  };
  asio::ip::tcp::socket& Lowest() {
    if (auto* tcp = std::get_if<asio::ip::tcp::socket>(&socket_)) return *tcp;
    return std::get<asio::ssl::stream<asio::ip::tcp::socket>>(socket_)
        .next_layer();
  }
  std::shared_ptr<Call> Find(uint64_t id) {
    return call_ && call_->id == id ? call_ : nullptr;
  }
  std::shared_ptr<Http1ByteStream> Stream() {
    return std::shared_ptr<Http1ByteStream>(
        new Http1ByteStream(shared_from_this(), call_->id));
  }
  void Stop(Status status);
  void Complete();
  void Deadline(Time deadline);
  Status Event(Http1Event event);
  asio::awaitable<Status> Handshake();
  asio::awaitable<Status> Reader();
  asio::awaitable<Status> Writer();
  asio::awaitable<Status> Queue(std::shared_ptr<Call> call, std::string bytes,
                                bool end = false);
  Socket socket_;
  asio::strand<asio::any_io_executor> strand_;
  Http1Role role_;
  Http1ConnectionOptions options_;
  std::string hostname_;
  Time handshake_deadline_;
  bool tls_ready_;
  std::unique_ptr<Http1Parser> parser_;
  std::shared_ptr<Call> call_;
  std::deque<Output> outgoing_;
  size_t outgoing_bytes_ = 0;
  uint64_t next_id_ = 1;
  Notification reader_event_;
  Notification writer_event_;
  Notification accept_event_;
  Notification stopped_event_;
  Notification joined_;
  asio::steady_timer shutdown_timer_;
  bool started_ = false;
  bool stopped_ = false;
  bool draining_ = false;
  bool accepting_ = false;
  bool writer_done_ = false;
  bool informational_ = false;
  bool saw_input_ = false;
  Status outcome_;
};
Status Http1Connection::Impl::Initialize() {
  if (options_.max_queued_bytes < 64 ||
      options_.limits.max_chunk_bytes > options_.max_queued_bytes - 64 ||
      options_.limits.max_metadata_bytes > options_.max_queued_bytes - 64)
    return {Code::kInvalidArgument,
            "HTTP/1.1 queue must fit metadata and chunks",
            {}};
  auto parser = Http1Parser::Create(role_, options_.limits);
  if (!parser.ok()) return parser.status();
  parser_ = std::move(*parser.value());
  return {};
}
void Http1Connection::Impl::Stop(Status status) {
  if (stopped_) return;
  stopped_ = true;
  outcome_ = status;
  shutdown_timer_.cancel();
  asio::error_code ignored;
  // Announce TCP EOF before close cancels outstanding overlapped I/O. On
  // Windows, closing directly can reset a completed response at the peer.
  Lowest().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
  Lowest().close(ignored);
  if (call_) {
    call_->timer.cancel();
    if ((!call_->ended || !call_->send_flushed) && !call_->failure) {
      if (call_->deadline <= std::chrono::steady_clock::now())
        call_->failure =
            Status{Code::kDeadlineExceeded, "deadline exceeded", {}};
      else
        call_->failure = status.ok() ? Closed() : status;
    }
    Notify(call_->read_event);
    Notify(call_->write_event);
    Notify(call_->close_event);
  }
  reader_event_.close();
  writer_event_.close();
  accept_event_.close();
  stopped_event_.close();
}
void Http1Connection::Impl::Complete() {
  if (!call_ || !call_->ended || !call_->send_flushed) return;
  call_->timer.cancel();
  Notify(call_->close_event);
  if (!call_->keep_alive || draining_) {
    Stop({});
  } else if (call_->released && !stopped_) {
    Status status = parser_->NextMessage();
    if (!status.ok()) {
      Stop(status);
      return;
    }
    call_.reset();
    saw_input_ = false;
    Notify(reader_event_);
  }
}
void Http1Connection::Impl::Deadline(Time deadline) {
  auto call = call_;
  call->timer.cancel();
  call->deadline = deadline;
  if (deadline == Time::max() || (call->ended && call->send_flushed)) return;
  call->timer.expires_at(deadline);
  std::weak_ptr<Impl> weak = shared_from_this();
  call->timer.async_wait(
      [weak, id = call->id, deadline](asio::error_code error) {
        if (!error)
          if (auto self = weak.lock())
            if (auto current = self->Find(id))
              if (current->deadline == deadline &&
                  !(current->ended && current->send_flushed))
                self->Stop({Code::kDeadlineExceeded, "deadline exceeded", {}});
      });
}
asio::awaitable<Status> Http1Connection::Impl::Handshake() {
  auto* tls = std::get_if<asio::ssl::stream<asio::ip::tcp::socket>>(&socket_);
  if (!tls) co_return Status{};
  if (tls_ready_)
    co_return NegotiatedTlsProtocol(*tls, TlsAlpn::kHttp1).status();
  if (client()) {
    Status status = ConfigureTlsClient(*tls, hostname_, TlsAlpn::kHttp1);
    if (!status.ok()) co_return status;
  }
  if (handshake_deadline_ <= std::chrono::steady_clock::now())
    co_return Status{
        Code::kDeadlineExceeded, "TLS handshake deadline exceeded", {}};
  asio::steady_timer timer(strand_);
  if (handshake_deadline_ != Time::max()) {
    timer.expires_at(handshake_deadline_);
    std::weak_ptr<Impl> weak = shared_from_this();
    timer.async_wait([weak](asio::error_code error) {
      if (!error)
        if (auto self = weak.lock())
          self->Stop(
              {Code::kDeadlineExceeded, "TLS handshake deadline exceeded", {}});
    });
  }
  auto [error] =
      co_await tls->async_handshake(client() ? asio::ssl::stream_base::client
                                             : asio::ssl::stream_base::server,
                                    asio::as_tuple(asio::use_awaitable));
  timer.cancel();
  if (stopped_) co_return outcome_;
  if (error) co_return IoError(error);
  co_return NegotiatedTlsProtocol(*tls, TlsAlpn::kHttp1).status();
}
Status Http1Connection::Impl::Event(Http1Event event) {
  if (event.kind == Http1Event::Kind::kHeaders) {
    if (client()) {
      informational_ = false;
      for (const auto& [name, value] : event.headers)
        if (name == ":status" && value.starts_with('1')) informational_ = true;
      if (informational_) return {};
      if (!call_) return {Code::kDataLoss, "unsolicited HTTP response", {}};
    } else {
      if (call_) return {Code::kInternal, "overlapping HTTP requests", {}};
      call_ =
          std::make_shared<Call>(strand_, role_, options_.limits, next_id_++);
    }
    call_->headers = std::move(event.headers);
    call_->got_headers = true;
    call_->keep_alive = event.keep_alive;
    Notify(call_->read_event);
    Notify(accept_event_);
  } else if (event.kind == Http1Event::Kind::kBody) {
    if (!call_ || informational_)
      return {Code::kDataLoss, "unexpected HTTP body", {}};
    if (!call_->discarding && !event.body.empty()) {
      call_->incoming_bytes += event.body.size();
      call_->incoming.push_back(std::move(event.body));
      Notify(call_->read_event);
    }
  } else {
    if (informational_) {
      informational_ = false;
      saw_input_ = false;
      return parser_->NextMessage();
    }
    if (!call_) return {Code::kDataLoss, "unexpected HTTP end", {}};
    call_->trailers = std::move(event.headers);
    call_->keep_alive = event.keep_alive;
    call_->ended = true;
    Notify(call_->read_event);
    Notify(reader_event_);
    Complete();
  }
  return {};
}
asio::awaitable<Status> Http1Connection::Impl::Reader() {
  std::vector<char> buffer(options_.limits.max_chunk_bytes);
  std::string_view pending;
  while (!stopped_) {
    if ((call_ && call_->ended && !pending.empty()) ||
        (call_ && !call_->discarding &&
         call_->incoming_bytes >
             options_.max_queued_bytes - options_.limits.max_chunk_bytes)) {
      if (!(co_await Wait(reader_event_))) break;
      continue;
    }
    if (!call_ || !call_->ended) {
      if (!pending.empty()) saw_input_ = true;
      auto parsed = parser_->ReadOne(pending);
      if (!parsed.ok()) co_return parsed.status();
      if (*parsed.value()) {
        Status status = Event(std::move(**parsed.value()));
        if (!status.ok()) co_return status;
        continue;
      }
    }
    if (!pending.empty()) continue;
    auto [error, count] = co_await std::visit(
        [&buffer](auto& socket) {
          return socket.async_read_some(asio::buffer(buffer),
                                        asio::as_tuple(asio::use_awaitable));
        },
        socket_);
    if (error) {
      if (stopped_) break;
      if (error != asio::error::eof) co_return IoError(error);
      if (!call_ && !saw_input_) co_return Status{};
      auto ended = parser_->EndInput();
      if (!ended.ok()) {
        // An early response has already reached the peer. Closing an upload
        // during the bounded discard phase cannot invalidate that response.
        if (call_ && call_->discarding && call_->send_flushed)
          co_return Status{};
        co_return ended.status();
      }
      if (*ended.value()) {
        Status status = Event(std::move(**ended.value()));
        if (!status.ok()) co_return status;
      }
      // A TCP half-close still permits the server to finish its response.
      // Observers learn that the peer stopped sending, but the response
      // remains writable. The RPC router may cancel a cooperative handler;
      // raw HTTP handlers may still finish a response after a TCP half-close.
      if (!client() && call_) {
        call_->peer_eof = true;
        Notify(call_->close_event);
      }
      while (!client() && call_ && !call_->send_flushed && !stopped_) {
        if (!(co_await Wait(reader_event_))) co_return Canceled();
      }
      co_return Status{};
    }
    pending = std::string_view(buffer.data(), count);
  }
  co_return stopped_ ? outcome_ : Canceled();
}
asio::awaitable<Status> Http1Connection::Impl::Writer() {
  co_await asio::this_coro::throw_if_cancelled(false);
  while (!stopped_) {
    if (outgoing_.empty()) {
      if (!(co_await Wait(writer_event_))) break;
      continue;
    }
    // Keep the in-flight bytes charged until the socket acknowledges them.
    // Keep queued headers and small bodies in one socket/TLS write. Taking
    // the existing queue preserves its byte bound, including in-flight data.
    std::deque<Output> batch;
    batch.swap(outgoing_);
    std::vector<asio::const_buffer> buffers;
    buffers.reserve(batch.size());
    size_t batch_bytes = 0;
    bool end = false;
    for (const auto& output : batch) {
      if (!output.bytes.empty()) buffers.push_back(asio::buffer(output.bytes));
      batch_bytes += output.bytes.size();
      end = end || output.end;
    }
    if (batch_bytes != 0) {
      auto [error, count] = co_await std::visit(
          [&buffers](auto& socket) {
            return asio::async_write(socket, buffers,
                                     asio::as_tuple(asio::use_awaitable));
          },
          socket_);
      (void)count;
      if (error) co_return IoError(error);
    }
    outgoing_bytes_ -= batch_bytes;
    if (call_) {
      if (end) call_->send_flushed = true;
      Notify(call_->write_event);
      Notify(reader_event_);
      Complete();
    }
  }
  co_return Status{};
}
asio::awaitable<Status> Http1Connection::Impl::Run() {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (started_)
    co_return Status{
        Code::kFailedPrecondition, "connection already running", {}};
  started_ = true;
  if (stopped_) co_return outcome_;
  Status status = co_await Handshake();
  if (!status.ok()) {
    Stop(status);
    co_return outcome_;
  }
  auto self = shared_from_this();
  asio::co_spawn(strand_, Writer(), [self](auto exception, Status result) {
    self->writer_done_ = true;
    if (exception) result = {Code::kInternal, "HTTP writer task failed", {}};
    if (!result.ok()) self->Stop(result);
    Notify(self->joined_);
  });
  status = co_await Reader();
  Stop(status);
  co_await asio::this_coro::reset_cancellation_state(
      asio::disable_cancellation());
  while (!writer_done_) co_await Wait(joined_);
  co_return outcome_;
}
asio::awaitable<Status> Http1Connection::Impl::Queue(std::shared_ptr<Call> call,
                                                     std::string bytes,
                                                     bool end) {
  while (!stopped_ &&
         (bytes.size() > options_.max_queued_bytes - outgoing_bytes_ ||
          outgoing_.size() >= 256)) {
    if (!(co_await Wait(call->write_event))) co_return Canceled();
  }
  if (stopped_) co_return call->failure.value_or(Closed());
  outgoing_bytes_ += bytes.size();
  outgoing_.push_back({std::move(bytes), end});
  Notify(writer_event_);
  co_return Status{};
}
asio::awaitable<Handle> Http1Connection::Impl::Open(Metadata headers,
                                                    Time deadline) {
  // Stream destruction posts its release onto this strand. co_spawn may
  // otherwise dispatch the next Open inline ahead of that queued release.
  co_await asio::post(strand_, asio::use_awaitable);
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none)
    co_return Handle(Canceled());
  if (!client())
    co_return Handle(
        Status{Code::kFailedPrecondition, "server cannot open requests", {}});
  if (stopped_ || draining_) co_return Handle(Closed());
  if (call_)
    co_return Handle(Status{
        Code::kResourceExhausted, "HTTP/1.1 connection already in use", {}});
  if (deadline <= std::chrono::steady_clock::now())
    co_return Handle(Status{Code::kDeadlineExceeded, "deadline exceeded", {}});
  auto call =
      std::make_shared<Call>(strand_, role_, options_.limits, next_id_++);
  auto encoded = call->encoder.Headers(headers);
  if (!encoded.ok()) co_return Handle(encoded.status());
  call_ = call;
  call->send_started = true;
  Deadline(deadline);
  Status status = co_await Queue(call, std::move(*encoded.value()));
  Notify(reader_event_);
  if (!status.ok()) {
    Stop(status);
    co_return Handle(status);
  }
  co_return Handle(Stream());
}
asio::awaitable<Handle> Http1Connection::Impl::Accept() {
  if (client())
    co_return Handle(
        Status{Code::kFailedPrecondition, "client cannot accept requests", {}});
  BusyGuard guard(accepting_);
  if (!guard.acquired()) co_return Handle(Busy());
  while (!stopped_ && !draining_) {
    if (call_ && call_->got_headers && !call_->accepted) {
      call_->accepted = true;
      co_return Handle(Stream());
    }
    if (!(co_await Wait(accept_event_))) break;
  }
  co_return Handle(stopped_ || draining_ ? Closed() : Canceled());
}
asio::awaitable<Result<Metadata>> Http1Connection::Impl::ReadHeaders(
    uint64_t id) {
  auto call = Find(id);
  if (!call) co_return Result<Metadata>(Closed());
  BusyGuard guard(call->reading);
  if (!guard.acquired()) co_return Result<Metadata>(Busy());
  while (!call->got_headers) {
    if (call->failure) co_return Result<Metadata>(*call->failure);
    if (!(co_await Wait(call->read_event)))
      co_return Result<Metadata>(Canceled());
  }
  co_return Result<Metadata>(call->headers);
}
asio::awaitable<Bytes> Http1Connection::Impl::Read(uint64_t id) {
  auto call = Find(id);
  if (!call) co_return Bytes(Closed());
  BusyGuard guard(call->reading);
  if (!guard.acquired()) co_return Bytes(Busy());
  for (;;) {
    if (!call->incoming.empty()) {
      std::string bytes = std::move(call->incoming.front());
      call->incoming.pop_front();
      call->incoming_bytes -= bytes.size();
      Notify(reader_event_);
      co_return Bytes(std::move(bytes));
    }
    if (call->ended) co_return Bytes(std::nullopt);
    if (call->failure) co_return Bytes(*call->failure);
    if (!(co_await Wait(call->read_event))) co_return Bytes(Canceled());
  }
}
asio::awaitable<Status> Http1Connection::Impl::SendHeaders(uint64_t id,
                                                           Metadata headers) {
  auto call = Find(id);
  if (!call) co_return Closed();
  BusyGuard guard(call->writing);
  if (!guard.acquired()) co_return Busy();
  if (call->failure) co_return *call->failure;
  auto encoded = call->encoder.Headers(headers, call->keep_alive && !draining_);
  if (!encoded.ok()) co_return encoded.status();
  call->send_started = true;
  co_return co_await Queue(call, std::move(*encoded.value()));
}
asio::awaitable<Status> Http1Connection::Impl::SendResponse(
    uint64_t id, HttpMessage response, Metadata trailers) {
  if (client())
    co_return Status{Code::kFailedPrecondition, "server response required", {}};
  Status status = co_await SendHeaders(id, std::move(response.headers));
  if (status.ok()) status = co_await Write(id, std::move(response.body));
  if (status.ok()) status = co_await CloseSend(id, std::move(trailers));
  co_return status;
}
asio::awaitable<Status> Http1Connection::Impl::Write(uint64_t id,
                                                     std::string bytes) {
  auto call = Find(id);
  if (!call) co_return Closed();
  BusyGuard guard(call->writing);
  if (!guard.acquired()) co_return Busy();
  if (call->failure) co_return *call->failure;
  if (!call->send_started || call->send_closed)
    co_return Status{
        Code::kFailedPrecondition, "HTTP send direction is not open", {}};
  std::string_view remaining(bytes);
  while (!remaining.empty()) {
    size_t count = std::min(remaining.size(), options_.limits.max_chunk_bytes);
    auto encoded = call->encoder.Body(remaining.substr(0, count));
    if (!encoded.ok()) co_return encoded.status();
    Status status = co_await Queue(call, std::move(*encoded.value()));
    if (!status.ok()) co_return status;
    remaining.remove_prefix(count);
  }
  co_return Status{};
}
asio::awaitable<Status> Http1Connection::Impl::CloseSend(uint64_t id,
                                                         Metadata trailers) {
  auto call = Find(id);
  if (!call) co_return Closed();
  BusyGuard guard(call->writing);
  if (!guard.acquired()) co_return Busy();
  if (call->failure) co_return *call->failure;
  auto encoded = call->encoder.End(trailers);
  if (!encoded.ok()) co_return encoded.status();
  call->send_closed = true;
  co_return co_await Queue(call, std::move(*encoded.value()), true);
}
asio::awaitable<Status> Http1Connection::Impl::FinishSend(uint64_t id) {
  auto call = Find(id);
  if (!call) co_return Closed();
  BusyGuard guard(call->writing);
  if (!guard.acquired()) co_return Busy();
  if (!call->send_closed)
    co_return Status{Code::kFailedPrecondition, "CloseSend is required", {}};
  while (!call->send_flushed) {
    if (call->failure) co_return *call->failure;
    if (!(co_await Wait(call->write_event))) co_return Canceled();
  }
  co_return Status{};
}
asio::awaitable<Status> Http1Connection::Impl::DiscardRead(uint64_t id) {
  auto call = Find(id);
  if (!call) co_return Closed();
  BusyGuard guard(call->reading);
  if (!guard.acquired()) co_return Busy();
  if (client() || !call->send_flushed)
    co_return Status{Code::kFailedPrecondition,
                     "discard requires a flushed server response",
                     {}};
  call->discarding = true;
  call->incoming.clear();
  call->incoming_bytes = 0;
  Notify(reader_event_);
  while (!call->ended && !stopped_) {
    if (!(co_await Wait(call->read_event))) co_return Canceled();
  }
  co_return Status{};
}
asio::awaitable<Result<Metadata>> Http1Connection::Impl::ReadTrailers(
    uint64_t id) {
  auto call = Find(id);
  if (!call) co_return Result<Metadata>(Closed());
  if (!call->ended || !call->incoming.empty())
    co_return Result<Metadata>(
        Status{Code::kFailedPrecondition, "read EOF before trailers", {}});
  co_return Result<Metadata>(call->trailers);
}
asio::awaitable<Result<Metadata>> Http1Connection::Impl::Finish(uint64_t id) {
  auto trailers = co_await ReadTrailers(id);
  if (!trailers.ok()) co_return trailers;
  Status status = co_await FinishSend(id);
  if (!status.ok()) co_return Result<Metadata>(status);
  co_return trailers;
}
asio::awaitable<Status> Http1Connection::Impl::WaitClosed(uint64_t id) {
  auto call = Find(id);
  if (!call) co_return Closed();
  BusyGuard guard(call->observing);
  if (!guard.acquired()) co_return Busy();
  while (!stopped_ && !call->peer_eof && !(call->ended && call->send_flushed))
    if (!(co_await Wait(call->close_event))) co_return Canceled();
  if (call->peer_eof && !call->send_flushed) co_return Canceled();
  co_return call->failure.value_or(Status{});
}
asio::awaitable<Status> Http1Connection::Impl::SetDeadline(uint64_t id,
                                                           Time deadline) {
  if (!Find(id)) co_return Closed();
  Deadline(deadline);
  co_return Status{};
}
asio::awaitable<Status> Http1Connection::Impl::CancelStream(uint64_t id) {
  auto call = Find(id);
  if (call && !(call->ended && call->send_flushed)) Stop(Canceled());
  co_return Status{};
}
void Http1Connection::Impl::Release(uint64_t id) {
  auto call = Find(id);
  if (!call) return;
  call->released = true;
  if (!(call->ended && call->send_flushed))
    Stop(Canceled());
  else
    Complete();
}
asio::awaitable<Status> Http1Connection::Impl::Cancel() {
  Stop(Canceled());
  co_return Status{};
}
asio::awaitable<Status> Http1Connection::Impl::Shutdown(Time deadline) {
  if (stopped_) co_return outcome_;
  if (draining_)
    co_return Status{
        Code::kFailedPrecondition, "shutdown already in progress", {}};
  draining_ = true;
  accept_event_.close();
  if (!call_ || (call_->ended && call_->send_flushed)) {
    Stop({});
    co_return outcome_;
  }
  shutdown_timer_.expires_at(deadline);
  std::weak_ptr<Impl> weak = shared_from_this();
  shutdown_timer_.async_wait([weak](asio::error_code error) {
    if (!error)
      if (auto self = weak.lock())
        self->Stop({Code::kDeadlineExceeded, "shutdown deadline exceeded", {}});
  });
  if (!(co_await Wait(stopped_event_)) && !stopped_) Stop(Canceled());
  co_return outcome_;
}
Http1Connection::Http1Connection(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
Http1Connection::~Http1Connection() = default;
Result<std::shared_ptr<Http1Connection>> Http1Connection::Create(
    asio::ip::tcp::socket socket, Http1Role role,
    Http1ConnectionOptions options) {
  auto impl = std::make_shared<Impl>(Socket(std::move(socket)), role, options,
                                     "", Time::max());
  Status status = impl->Initialize();
  if (!status.ok()) return Result<std::shared_ptr<Http1Connection>>(status);
  return Result<std::shared_ptr<Http1Connection>>(
      std::shared_ptr<Http1Connection>(new Http1Connection(impl)));
}
Result<std::shared_ptr<Http1Connection>> Http1Connection::CreateTls(
    asio::ssl::stream<asio::ip::tcp::socket> socket, Http1Role role,
    std::string hostname, Http1ConnectionOptions options, Time deadline) {
  if (!SSL_in_before(socket.native_handle()))
    return Result<std::shared_ptr<Http1Connection>>(
        Status{Code::kFailedPrecondition,
               "TLS stream handshake has already started",
               {}});
  auto impl = std::make_shared<Impl>(Socket(std::move(socket)), role, options,
                                     std::move(hostname), deadline);
  Status status = impl->Initialize();
  if (!status.ok()) return Result<std::shared_ptr<Http1Connection>>(status);
  return Result<std::shared_ptr<Http1Connection>>(
      std::shared_ptr<Http1Connection>(new Http1Connection(impl)));
}
Result<std::shared_ptr<Http1Connection>> Http1Connection::AdoptTls(
    asio::ssl::stream<asio::ip::tcp::socket> socket, Http1Role role,
    Http1ConnectionOptions options) {
  auto impl = std::make_shared<Impl>(Socket(std::move(socket)), role, options,
                                     "", Time::max(), true);
  Status status = impl->Initialize();
  if (!status.ok()) return Result<std::shared_ptr<Http1Connection>>(status);
  return Result<std::shared_ptr<Http1Connection>>(
      std::shared_ptr<Http1Connection>(new Http1Connection(std::move(impl))));
}
asio::awaitable<Status> Http1Connection::Run() {
  return Dispatch(impl_, impl_->Run());
}
asio::awaitable<Handle> Http1Connection::Open(Metadata headers, Time deadline) {
  return Dispatch(impl_, impl_->Open(std::move(headers), deadline));
}
asio::awaitable<Handle> Http1Connection::Accept() {
  return Dispatch(impl_, impl_->Accept());
}
asio::awaitable<Status> Http1Connection::Shutdown(Time deadline) {
  return Dispatch(impl_, impl_->Shutdown(deadline));
}
asio::awaitable<Status> Http1Connection::Cancel() {
  return Dispatch(impl_, impl_->Cancel(), false);
}
Http1ByteStream::Http1ByteStream(
    std::shared_ptr<Http1Connection::Impl> connection, uint64_t id)
    : connection_(std::move(connection)), id_(id) {}
Http1ByteStream::~Http1ByteStream() {
  auto connection = std::move(connection_);
  auto executor = connection->executor();
  asio::post(executor, [connection = std::move(connection), id = id_] {
    connection->Release(id);
  });
}
asio::any_io_executor Http1ByteStream::executor() const {
  return connection_->executor();
}
asio::awaitable<Result<Metadata>> Http1ByteStream::ReadHeaders() {
  return Dispatch(connection_, connection_->ReadHeaders(id_), true,
                  connection_->client() ? id_ : 0);
}
asio::awaitable<Bytes> Http1ByteStream::Read() {
  return Dispatch(connection_, connection_->Read(id_), true,
                  connection_->client() ? id_ : 0);
}
asio::awaitable<Status> Http1ByteStream::SendHeaders(Metadata headers) {
  return Dispatch(connection_,
                  connection_->SendHeaders(id_, std::move(headers)), true, id_);
}
asio::awaitable<Status> Http1ByteStream::Write(std::string bytes) {
  return Dispatch(connection_, connection_->Write(id_, std::move(bytes)), true,
                  id_);
}
asio::awaitable<Status> Http1ByteStream::SendResponse(HttpMessage response,
                                                      Metadata trailers) {
  return Dispatch(
      connection_,
      connection_->SendResponse(id_, std::move(response), std::move(trailers)),
      true, id_);
}
asio::awaitable<Status> Http1ByteStream::CloseSend(Metadata trailers) {
  return Dispatch(connection_, connection_->CloseSend(id_, std::move(trailers)),
                  true, id_);
}
asio::awaitable<Status> Http1ByteStream::FinishSend() {
  return Dispatch(connection_, connection_->FinishSend(id_), true, id_);
}
asio::awaitable<Status> Http1ByteStream::DiscardRead() {
  return Dispatch(connection_, connection_->DiscardRead(id_), true, id_);
}
asio::awaitable<Result<Metadata>> Http1ByteStream::Finish() {
  return Dispatch(connection_, connection_->Finish(id_), true, id_);
}
asio::awaitable<Result<Metadata>> Http1ByteStream::ReadTrailers() {
  return Dispatch(connection_, connection_->ReadTrailers(id_), true, id_);
}
asio::awaitable<Status> Http1ByteStream::WaitClosed() {
  return Dispatch(connection_, connection_->WaitClosed(id_));
}
asio::awaitable<Status> Http1ByteStream::SetDeadline(Time deadline) {
  return Dispatch(connection_, connection_->SetDeadline(id_, deadline), true,
                  id_);
}
asio::awaitable<Status> Http1ByteStream::Cancel() {
  return Dispatch(connection_, connection_->CancelStream(id_), false);
}
}  // namespace connectrpc
