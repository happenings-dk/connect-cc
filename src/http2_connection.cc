#include "connectrpc/http2_connection.h"

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
#include <map>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "connectrpc/tls.h"

namespace connectrpc {
namespace {
using Notification = asio::experimental::channel<void(asio::error_code)>;
using ByteResult = Result<std::optional<std::string>>;
using StreamResult = Result<std::shared_ptr<Http2ByteStream>>;
using Socket = std::variant<asio::ip::tcp::socket,
                            asio::ssl::stream<asio::ip::tcp::socket>>;
Status Canceled() { return {Code::kCanceled, "operation canceled", {}}; }
Status Unexpected() {
  return {Code::kInternal, "unexpected coroutine failure", {}};
}
Status IoError(asio::error_code error) {
  return {error == asio::error::operation_aborted ? Code::kCanceled
                                                  : Code::kUnavailable,
          error.message(),
          {}};
}
// Guards only run on the connection strand and always outlive the suspended
// operation. A second concurrent operation is rejected instead of racing.
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
Status Busy() {
  return {Code::kFailedPrecondition,
          "concurrent operation on the same stream direction",
          {}};
}
// Preserve cancellation that happened before co_spawn installs the child's
// slot. Asio propagates subsequent cancellation; this check closes the handoff
// gap.
template <typename T>
asio::awaitable<T> Dispatch(asio::any_io_executor executor,
                            asio::awaitable<T> operation,
                            bool honor_existing_cancellation = true) {
  co_await asio::this_coro::throw_if_cancelled(false);
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (honor_existing_cancellation &&
      cancellation.cancelled() != asio::cancellation_type::none) {
    if constexpr (std::is_same_v<T, Status>)
      co_return Canceled();
    else
      co_return T(Canceled());
  }
  auto [exception, result] = co_await asio::co_spawn(
      executor, std::move(operation), asio::as_tuple(asio::use_awaitable));
  if (exception) {
    if constexpr (std::is_same_v<T, Status>)
      co_return Unexpected();
    else
      co_return T(Unexpected());
  }
  co_return std::move(result);
}
template <typename Connection, typename T>
asio::awaitable<T> DispatchConnection(std::shared_ptr<Connection> connection,
                                      asio::awaitable<T> operation,
                                      bool honor_existing_cancellation = true) {
  co_return co_await Dispatch(connection->executor(), std::move(operation),
                              honor_existing_cancellation);
}
template <typename Connection, typename T>
asio::awaitable<T> DispatchStream(std::shared_ptr<Connection> connection,
                                  int32_t id, asio::awaitable<T> operation,
                                  bool reset_on_cancel = true) {
  T result = co_await Dispatch(connection->executor(), std::move(operation));
  Status status;
  if constexpr (std::is_same_v<T, Status>)
    status = result;
  else
    status = result.status();
  if (reset_on_cancel && status.code == Code::kCanceled)
    co_await Dispatch(connection->executor(), connection->CancelStream(id),
                      false);
  co_return std::move(result);
}
}  // namespace

class Http2Connection::Impl
    : public std::enable_shared_from_this<Http2Connection::Impl> {
 public:
  Impl(Socket socket, Http2Session::Role role, std::string hostname,
       std::chrono::steady_clock::time_point handshake_deadline,
       bool tls_ready = false)
      : socket_(std::move(socket)),
        strand_(asio::make_strand(Lowest().get_executor())),
        role_(role),
        hostname_(std::move(hostname)),
        handshake_deadline_(handshake_deadline),
        tls_ready_(tls_ready),
        wake_(strand_, 1),
        accepted_(strand_, 1),
        stopped_event_(strand_, 1),
        writer_joined_(strand_, 1),
        shutdown_timer_(strand_) {}
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Status Initialize(Http2StreamOptions options);
  auto executor() const { return strand_; }
  bool client() const { return role_ == Http2Session::Role::kClient; }
  asio::awaitable<Status> Run();
  asio::awaitable<StreamResult> Open(
      Metadata headers, std::chrono::steady_clock::time_point deadline);
  asio::awaitable<StreamResult> Accept();
  asio::awaitable<Status> Shutdown(
      std::chrono::steady_clock::time_point deadline);
  asio::awaitable<Status> Cancel();
  asio::awaitable<Result<Metadata>> ReadHeaders(int32_t id);
  asio::awaitable<ByteResult> Read(int32_t id);
  asio::awaitable<Status> FinishSend(int32_t id);
  asio::awaitable<Status> DiscardRead(int32_t id);
  asio::awaitable<Result<Metadata>> Finish(int32_t id);
  asio::awaitable<Result<Metadata>> ReadTrailers(int32_t id);
  asio::awaitable<Status> WaitClosed(int32_t id);
  asio::awaitable<Status> Write(int32_t id, std::string bytes);
  asio::awaitable<Status> SendHeaders(int32_t id, Metadata headers,
                                      bool wake = true);
  asio::awaitable<Status> SendResponse(int32_t id, HttpMessage response,
                                       Metadata trailers);
  asio::awaitable<Status> CloseSend(int32_t id, Metadata trailers);
  asio::awaitable<Status> SetDeadline(
      int32_t id, std::chrono::steady_clock::time_point deadline);
  asio::awaitable<Status> CancelStream(int32_t id);
  void Release(int32_t id);

 private:
  struct Call {
    explicit Call(asio::any_io_executor executor)
        : read_event(executor, 1),
          write_event(executor, 1),
          close_event(executor, 1),
          timer(executor) {}
    Metadata headers;
    Metadata trailers;
    std::deque<std::string> incoming;
    Notification read_event;
    Notification write_event;
    Notification close_event;
    asio::steady_timer timer;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();
    std::optional<Status> terminal;
    std::optional<Status> send_failure;
    bool got_headers = false;
    bool ended = false;
    bool reading = false;
    bool writing = false;
    bool observing = false;
    bool send_closed = false;
    bool send_flushed = false;
  };
  asio::ip::tcp::socket& Lowest() {
    if (auto* tcp = std::get_if<asio::ip::tcp::socket>(&socket_)) return *tcp;
    return std::get<asio::ssl::stream<asio::ip::tcp::socket>>(socket_)
        .next_layer();
  }
  std::shared_ptr<Call> Find(int32_t id) {
    auto found = calls_.find(id);
    return found == calls_.end() ? nullptr : found->second;
  }
  void Wake() { wake_.try_send(asio::error_code{}); }
  void Abort(int32_t id, Status status);
  void Stop(Status status);
  bool AllClosed() const;
  void Deadline(int32_t id, std::chrono::steady_clock::time_point deadline);
  asio::awaitable<Status> Handshake();
  asio::awaitable<Status> Writer();
  asio::awaitable<bool> Wait(Notification& event, int32_t id,
                             bool reset_on_cancel = true);
  std::shared_ptr<Http2ByteStream> Handle(int32_t id) {
    return std::shared_ptr<Http2ByteStream>(
        new Http2ByteStream(shared_from_this(), id));
  }
  Socket socket_;
  asio::strand<asio::any_io_executor> strand_;
  Http2Session::Role role_;
  std::string hostname_;
  std::chrono::steady_clock::time_point handshake_deadline_;
  bool tls_ready_;
  std::unique_ptr<Http2StreamSession> session_;
  std::map<int32_t, std::shared_ptr<Call>> calls_;
  std::deque<int32_t> pending_;
  std::vector<int32_t> serialized_ends_;
  Notification wake_;
  Notification accepted_;
  Notification stopped_event_;
  Notification writer_joined_;
  asio::steady_timer shutdown_timer_;
  bool started_ = false;
  bool stopping_ = false;
  bool draining_ = false;
  bool accepting_ = false;
  bool writer_done_ = false;
  Status outcome_;
};

Status Http2Connection::Impl::Initialize(Http2StreamOptions options) {
  std::weak_ptr<Impl> weak = shared_from_this();
  Http2StreamEvents events;
  events.headers = [weak](int32_t id, Metadata headers, bool trailers,
                          bool informational) {
    auto self = weak.lock();
    if (!self || informational) return;
    auto call = self->Find(id);
    if (!call && self->role_ == Http2Session::Role::kServer) {
      call = std::make_shared<Call>(self->strand_);
      self->calls_.emplace(id, call);
      self->pending_.push_back(id);
      self->accepted_.try_send(asio::error_code{});
    }
    if (!call) return;
    if (trailers)
      call->trailers = std::move(headers);
    else {
      call->headers = std::move(headers);
      call->got_headers = true;
    }
    call->read_event.try_send(asio::error_code{});
  };
  events.data = [weak](int32_t id, std::string_view bytes) {
    auto self = weak.lock();
    if (!self) return;
    auto call = self->Find(id);
    if (!call) {
      self->session_->Reset(id);
      return;
    }
    while (!bytes.empty()) {
      if (call->incoming.empty() || call->incoming.back().size() == 16384)
        call->incoming.emplace_back();
      size_t count =
          std::min<size_t>(16384 - call->incoming.back().size(), bytes.size());
      call->incoming.back().append(bytes.substr(0, count));
      bytes.remove_prefix(count);
    }
    call->read_event.try_send(asio::error_code{});
  };
  events.end = [weak](int32_t id) {
    if (auto self = weak.lock())
      if (auto call = self->Find(id)) {
        call->ended = true;
        call->read_event.try_send(asio::error_code{});
      }
  };
  events.close = [weak](int32_t id, Status status) {
    if (auto self = weak.lock()) {
      if (auto call = self->Find(id)) {
        if (!call->terminal) {
          // A peer reset and an expired timer can be ready in the same event
          // loop iteration. Do not let handler dispatch order change an
          // already-expired call from deadline_exceeded to canceled.
          if (!status.ok() &&
              call->deadline <= std::chrono::steady_clock::now())
            status = {Code::kDeadlineExceeded, "deadline exceeded", {}};
          call->terminal = std::move(status);
        }
        if (call->send_flushed) call->timer.cancel();
        call->read_event.try_send(asio::error_code{});
        call->write_event.try_send(asio::error_code{});
        call->close_event.try_send(asio::error_code{});
      } else {
        // A malformed header block can fail before creating an application
        // call.
        self->session_->Release(id);
      }
      self->Wake();
    }
  };
  events.writable = [weak](int32_t id) {
    if (auto self = weak.lock())
      if (auto call = self->Find(id))
        call->write_event.try_send(asio::error_code{});
  };
  events.send_end = [weak](int32_t id) {
    if (auto self = weak.lock()) self->serialized_ends_.push_back(id);
  };
  auto created = Http2StreamSession::Create(role_, options, std::move(events));
  if (!created.ok()) return created.status();
  session_ = std::move(*created.value());
  return {};
}
bool Http2Connection::Impl::AllClosed() const {
  for (const auto& [id, call] : calls_)
    if (!call->terminal) return false;
  return true;
}
void Http2Connection::Impl::Stop(Status status) {
  if (stopping_) return;
  stopping_ = true;
  outcome_ = status;
  shutdown_timer_.cancel();
  asio::error_code ignored;
  // Preserve completed writes when close cancels the outstanding reader,
  // including Winsock's overlapped receive operation.
  Lowest().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
  Lowest().close(ignored);
  session_->Fail(status.ok()
                     ? Status{Code::kUnavailable, "connection closed", {}}
                     : status);
  for (const auto& [id, call] : calls_) {
    if (!call->send_flushed && !call->send_failure)
      call->send_failure = status.ok()
                               ? Status{Code::kUnavailable,
                                        "connection stopped before flush",
                                        {}}
                               : status;
    call->read_event.try_send(asio::error_code{});
    call->write_event.try_send(asio::error_code{});
  }
  wake_.close();
  accepted_.close();
  stopped_event_.close();
}
void Http2Connection::Impl::Abort(int32_t id, Status status) {
  auto call = Find(id);
  if (!call) return;
  if (call->terminal) {
    if (call->terminal->ok() && !call->send_flushed && !call->send_failure) {
      call->send_failure = std::move(status);
      call->write_event.try_send(asio::error_code{});
    }
    return;
  }
  call->terminal = std::move(status);
  session_->Reset(id);
  Wake();
}
void Http2Connection::Impl::Deadline(
    int32_t id, std::chrono::steady_clock::time_point deadline) {
  auto call = Find(id);
  if (!call ||
      (call->terminal && (!call->terminal->ok() || call->send_flushed)))
    return;
  call->timer.cancel();
  call->deadline = deadline;
  if (deadline == std::chrono::steady_clock::time_point::max()) return;
  call->timer.expires_at(deadline);
  std::weak_ptr<Impl> weak = shared_from_this();
  call->timer.async_wait([weak, id](asio::error_code error) {
    if (!error)
      if (auto self = weak.lock())
        self->Abort(id, {Code::kDeadlineExceeded, "deadline exceeded", {}});
  });
}
asio::awaitable<bool> Http2Connection::Impl::Wait(Notification& event,
                                                  int32_t id,
                                                  bool reset_on_cancel) {
  auto [error] =
      co_await event.async_receive(asio::as_tuple(asio::use_awaitable));
  if (error) {
    if (reset_on_cancel) Abort(id, Canceled());
    co_return false;
  }
  co_return true;
}
asio::awaitable<Status> Http2Connection::Impl::Handshake() {
  auto* tls = std::get_if<asio::ssl::stream<asio::ip::tcp::socket>>(&socket_);
  if (!tls) co_return Status{};
  if (tls_ready_) co_return CheckHttp2Alpn(*tls);
  if (role_ == Http2Session::Role::kClient) {
    Status status = ConfigureTlsClient(*tls, hostname_);
    if (!status.ok()) co_return status;
  }
  if (handshake_deadline_ <= std::chrono::steady_clock::now())
    co_return Status{
        Code::kDeadlineExceeded, "TLS handshake deadline exceeded", {}};
  asio::steady_timer timer(strand_);
  if (handshake_deadline_ != std::chrono::steady_clock::time_point::max()) {
    timer.expires_at(handshake_deadline_);
    std::weak_ptr<Impl> weak = shared_from_this();
    timer.async_wait([weak](asio::error_code error) {
      if (!error)
        if (auto self = weak.lock())
          self->Stop(
              {Code::kDeadlineExceeded, "TLS handshake deadline exceeded", {}});
    });
  }
  auto [error] = co_await tls->async_handshake(
      role_ == Http2Session::Role::kClient ? asio::ssl::stream_base::client
                                           : asio::ssl::stream_base::server,
      asio::as_tuple(asio::use_awaitable));
  timer.cancel();
  if (stopping_) co_return outcome_;
  if (error) co_return IoError(error);
  co_return CheckHttp2Alpn(*tls);
}
asio::awaitable<Status> Http2Connection::Impl::Writer() {
  co_await asio::this_coro::throw_if_cancelled(false);
  while (!stopping_) {
    auto output = session_->Drain();
    if (!output.ok()) co_return output.status();
    std::vector<int32_t> ended = std::exchange(serialized_ends_, {});
    if (!output.value()->empty()) {
      auto [error, count] = co_await std::visit(
          [&output](auto& socket) {
            return asio::async_write(socket, asio::buffer(*output.value()),
                                     asio::as_tuple(asio::use_awaitable));
          },
          socket_);
      (void)count;
      if (error) co_return IoError(error);
    }
    for (int32_t id : ended) {
      if (auto call = Find(id)) {
        call->send_flushed = true;
        if (call->terminal) call->timer.cancel();
        call->write_event.try_send(asio::error_code{});
      }
    }
    if (output.value()->empty()) {
      if (draining_ && AllClosed()) {
        Stop({});
        break;
      }
      auto [error] =
          co_await wake_.async_receive(asio::as_tuple(asio::use_awaitable));
      if (error) break;
    }
  }
  co_return Status{};
}
asio::awaitable<Status> Http2Connection::Impl::Run() {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (started_)
    co_return Status{
        Code::kFailedPrecondition, "connection already running", {}};
  started_ = true;
  if (stopping_) co_return outcome_;
  Status status = co_await Handshake();
  if (!status.ok()) {
    Stop(status);
    co_return status;
  }
  auto self = shared_from_this();
  asio::co_spawn(strand_, Writer(), [self](auto exception, Status status) {
    if (exception) status = Unexpected();
    if (!status.ok()) self->Stop(status);
    self->writer_done_ = true;
    self->writer_joined_.try_send(asio::error_code{});
  });
  while (!stopping_) {
    char buffer[16384];
    auto [error, count] = co_await std::visit(
        [&buffer](auto& socket) {
          return socket.async_read_some(asio::buffer(buffer),
                                        asio::as_tuple(asio::use_awaitable));
        },
        socket_);
    if (error) {
      Stop(error == asio::error::eof && AllClosed() ? Status{}
                                                    : IoError(error));
      break;
    }
    status = session_->Receive(std::string_view(buffer, count));
    if (!status.ok()) {
      Stop(status);
      break;
    }
    Wake();
  }
  co_await asio::this_coro::reset_cancellation_state(
      asio::disable_cancellation());
  if (!writer_done_)
    co_await writer_joined_.async_receive(asio::as_tuple(asio::use_awaitable));
  co_return outcome_;
}
asio::awaitable<StreamResult> Http2Connection::Impl::Open(
    Metadata headers, std::chrono::steady_clock::time_point deadline) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (stopping_)
    co_return StreamResult({Code::kUnavailable, "connection closed", {}});
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none)
    co_return StreamResult(Canceled());
  if (deadline <= std::chrono::steady_clock::now())
    co_return StreamResult({Code::kDeadlineExceeded, "deadline exceeded", {}});
  auto id = session_->StartRequest(std::move(headers));
  if (!id.ok()) co_return StreamResult(id.status());
  calls_.emplace(*id.value(), std::make_shared<Call>(strand_));
  Deadline(*id.value(), deadline);
  Wake();
  co_return StreamResult(Handle(*id.value()));
}
asio::awaitable<StreamResult> Http2Connection::Impl::Accept() {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (role_ != Http2Session::Role::kServer)
    co_return StreamResult({Code::kFailedPrecondition, "not a server", {}});
  BusyGuard guard(accepting_);
  if (!guard.acquired()) co_return StreamResult(Busy());
  while (pending_.empty()) {
    if (stopping_ || draining_)
      co_return StreamResult(
          {Code::kUnavailable, "connection draining or closed", {}});
    auto [error] =
        co_await accepted_.async_receive(asio::as_tuple(asio::use_awaitable));
    if (error)
      co_return StreamResult(
          error == asio::error::operation_aborted
              ? Canceled()
              : Status{Code::kUnavailable, "connection closed", {}});
  }
  int32_t id = pending_.front();
  pending_.pop_front();
  co_return StreamResult(Handle(id));
}
asio::awaitable<Result<Metadata>> Http2Connection::Impl::ReadHeaders(
    int32_t id) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto call = Find(id);
  if (!call) co_return Result<Metadata>(Canceled());
  BusyGuard guard(call->reading);
  if (!guard.acquired()) co_return Result<Metadata>(Busy());
  while (!call->got_headers) {
    if (call->terminal)
      co_return Result<Metadata>(
          call->terminal->ok() ? Status{Code::kInternal, "missing headers", {}}
                               : *call->terminal);
    if (!co_await Wait(call->read_event, id, client()))
      co_return Result<Metadata>(Canceled());
  }
  co_return Result<Metadata>(call->headers);
}
asio::awaitable<ByteResult> Http2Connection::Impl::Read(int32_t id) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto call = Find(id);
  if (!call) co_return ByteResult(Canceled());
  BusyGuard guard(call->reading);
  if (!guard.acquired()) co_return ByteResult(Busy());
  for (;;) {
    if (call->terminal && (call->terminal->code == Code::kCanceled ||
                           call->terminal->code == Code::kDeadlineExceeded))
      co_return ByteResult(*call->terminal);
    if (!call->incoming.empty()) {
      std::string bytes = std::move(call->incoming.front());
      call->incoming.pop_front();
      Status status = session_->Consume(id, bytes.size());
      if (!status.ok()) co_return ByteResult(status);
      Wake();
      co_return ByteResult(std::optional<std::string>(std::move(bytes)));
    }
    if (call->terminal && !call->terminal->ok())
      co_return ByteResult(*call->terminal);
    if (call->ended) co_return ByteResult(std::optional<std::string>());
    if (!co_await Wait(call->read_event, id, client()))
      co_return ByteResult(Canceled());
  }
}
asio::awaitable<Status> Http2Connection::Impl::WaitClosed(int32_t id) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto call = Find(id);
  if (!call) co_return Canceled();
  BusyGuard guard(call->observing);
  if (!guard.acquired()) co_return Busy();
  while (!call->terminal) {
    if (!co_await Wait(call->close_event, id, false)) co_return Canceled();
  }
  co_return *call->terminal;
}
asio::awaitable<Status> Http2Connection::Impl::FinishSend(int32_t id) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto call = Find(id);
  if (!call) co_return Canceled();
  BusyGuard guard(call->writing);
  if (!guard.acquired()) co_return Busy();
  if (!call->send_closed)
    co_return Status{
        Code::kFailedPrecondition, "CloseSend before FinishSend", {}};
  while (!call->send_flushed) {
    if (call->send_failure) co_return *call->send_failure;
    if (call->terminal && !call->terminal->ok()) co_return *call->terminal;
    if (stopping_)
      co_return outcome_.ok()
          ? Status{Code::kUnavailable, "connection stopped before flush", {}}
          : outcome_;
    if (!co_await Wait(call->write_event, id)) co_return Canceled();
  }
  if (call->send_failure) co_return *call->send_failure;
  co_return Status{};
}
asio::awaitable<Status> Http2Connection::Impl::DiscardRead(int32_t id) {
  auto call = Find(id);
  if (!call) co_return Canceled();
  if (role_ != Http2Session::Role::kServer || !call->send_flushed)
    co_return Status{Code::kFailedPrecondition,
                     "flush server response before DiscardRead",
                     {}};
  if (call->reading) co_return Busy();
  for (const auto& bytes : call->incoming) {
    Status consumed = session_->Consume(id, bytes.size());
    if (!consumed.ok()) co_return consumed;
  }
  call->incoming.clear();
  call->ended = true;
  if (!call->terminal) {
    // RFC 9113 permits a server to stop the request with NO_ERROR after its
    // complete response. The client must retain that response's status/body.
    call->terminal = Status{};
    Status reset = session_->Reset(id, 0);
    if (!reset.ok()) co_return reset;
  }
  Wake();
  co_return Status{};
}
asio::awaitable<Result<Metadata>> Http2Connection::Impl::ReadTrailers(
    int32_t id) {
  auto call = Find(id);
  if (!call) co_return Result<Metadata>(Canceled());
  BusyGuard guard(call->reading);
  if (!guard.acquired()) co_return Result<Metadata>(Busy());
  if (call->terminal && !call->terminal->ok())
    co_return Result<Metadata>(*call->terminal);
  if (!call->incoming.empty() || !call->ended)
    co_return Result<Metadata>(
        {Code::kFailedPrecondition, "read through EOF before trailers", {}});
  co_return Result<Metadata>(call->trailers);
}
asio::awaitable<Result<Metadata>> Http2Connection::Impl::Finish(int32_t id) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto call = Find(id);
  if (!call) co_return Result<Metadata>(Canceled());
  BusyGuard guard(call->reading);
  if (!guard.acquired()) co_return Result<Metadata>(Busy());
  if (call->terminal && !call->terminal->ok())
    co_return Result<Metadata>(*call->terminal);
  if (!call->incoming.empty() || !call->ended)
    co_return Result<Metadata>(
        {Code::kFailedPrecondition, "read through EOF before Finish", {}});
  while (!call->terminal)
    if (!co_await Wait(call->read_event, id))
      co_return Result<Metadata>(Canceled());
  if (!call->terminal->ok()) co_return Result<Metadata>(*call->terminal);
  co_return Result<Metadata>(call->trailers);
}
asio::awaitable<Status> Http2Connection::Impl::Write(int32_t id,
                                                     std::string bytes) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto call = Find(id);
  if (!call) co_return Canceled();
  BusyGuard guard(call->writing);
  if (!guard.acquired()) co_return Busy();
  if (call->terminal && !call->terminal->ok()) co_return *call->terminal;
  if (bytes.empty()) {
    auto written = session_->TryWrite(id, "");
    co_return written.status();
  }
  std::string_view remaining = bytes;
  while (!remaining.empty()) {
    if (call->terminal)
      co_return call->terminal->ok()
          ? Status{Code::kFailedPrecondition, "stream closed", {}}
          : *call->terminal;
    auto written = session_->TryWrite(id, remaining);
    if (!written.ok()) co_return written.status();
    remaining.remove_prefix(*written.value());
    Wake();
    if (*written.value() == 0 && !co_await Wait(call->write_event, id))
      co_return Canceled();
  }
  co_return Status{};
}
asio::awaitable<Status> Http2Connection::Impl::SendHeaders(int32_t id,
                                                           Metadata headers,
                                                           bool wake) {
  auto call = Find(id);
  if (!call) co_return Canceled();
  if (call->writing) co_return Busy();
  if (call->terminal && !call->terminal->ok()) co_return *call->terminal;
  Status status = session_->StartResponse(id, std::move(headers));
  if (wake) Wake();
  co_return status;
}
asio::awaitable<Status> Http2Connection::Impl::SendResponse(
    int32_t id, HttpMessage response, Metadata trailers) {
  if (client())
    co_return Status{Code::kFailedPrecondition, "server response required", {}};
  Status status = co_await SendHeaders(id, std::move(response.headers), false);
  if (status.ok()) status = co_await Write(id, std::move(response.body));
  if (status.ok()) status = co_await CloseSend(id, std::move(trailers));
  co_return status;
}
asio::awaitable<Status> Http2Connection::Impl::CloseSend(int32_t id,
                                                         Metadata trailers) {
  auto call = Find(id);
  if (!call) co_return Canceled();
  if (call->writing) co_return Busy();
  if (call->terminal && !call->terminal->ok()) co_return *call->terminal;
  Status status = session_->CloseSend(id, std::move(trailers));
  if (status.ok()) call->send_closed = true;
  Wake();
  co_return status;
}
asio::awaitable<Status> Http2Connection::Impl::SetDeadline(
    int32_t id, std::chrono::steady_clock::time_point deadline) {
  if (!Find(id)) co_return Canceled();
  Deadline(id, deadline);
  co_return Status{};
}
asio::awaitable<Status> Http2Connection::Impl::CancelStream(int32_t id) {
  Abort(id, Canceled());
  co_return Status{};
}
void Http2Connection::Impl::Release(int32_t id) {
  auto call = Find(id);
  if (!call) return;
  Abort(id, Canceled());
  session_->Release(id);
  calls_.erase(id);
  Wake();
}
asio::awaitable<Status> Http2Connection::Impl::Cancel() {
  Stop(Canceled());
  co_return Status{};
}
asio::awaitable<Status> Http2Connection::Impl::Shutdown(
    std::chrono::steady_clock::time_point deadline) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (stopping_) co_return outcome_;
  if (draining_)
    co_return Status{
        Code::kFailedPrecondition, "shutdown already in progress", {}};
  draining_ = true;
  Status status = session_->Shutdown();
  if (!status.ok()) {
    Stop(status);
    co_return status;
  }
  accepted_.close();
  shutdown_timer_.expires_at(deadline);
  std::weak_ptr<Impl> weak = shared_from_this();
  shutdown_timer_.async_wait([weak](asio::error_code error) {
    if (!error)
      if (auto self = weak.lock())
        self->Stop({Code::kDeadlineExceeded, "shutdown deadline exceeded", {}});
  });
  Wake();
  auto [error] = co_await stopped_event_.async_receive(
      asio::as_tuple(asio::use_awaitable));
  if (error == asio::error::operation_aborted) Stop(Canceled());
  co_return outcome_;
}

Http2Connection::Http2Connection(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
Http2Connection::~Http2Connection() = default;
Result<std::shared_ptr<Http2Connection>> Http2Connection::Create(
    asio::ip::tcp::socket socket, Http2Session::Role role,
    Http2StreamOptions options) {
  auto impl =
      std::make_shared<Impl>(Socket(std::move(socket)), role, "",
                             std::chrono::steady_clock::time_point::max());
  Status status = impl->Initialize(options);
  if (!status.ok()) return Result<std::shared_ptr<Http2Connection>>(status);
  return Result<std::shared_ptr<Http2Connection>>(
      std::shared_ptr<Http2Connection>(new Http2Connection(std::move(impl))));
}
Result<std::shared_ptr<Http2Connection>> Http2Connection::CreateTls(
    asio::ssl::stream<asio::ip::tcp::socket> socket, Http2Session::Role role,
    std::string hostname, Http2StreamOptions options,
    std::chrono::steady_clock::time_point deadline) {
  if (!SSL_in_before(socket.native_handle()))
    return Result<std::shared_ptr<Http2Connection>>(
        Status{Code::kFailedPrecondition,
               "TLS stream handshake has already started",
               {}});
  auto impl = std::make_shared<Impl>(Socket(std::move(socket)), role,
                                     std::move(hostname), deadline);
  Status status = impl->Initialize(options);
  if (!status.ok()) return Result<std::shared_ptr<Http2Connection>>(status);
  return Result<std::shared_ptr<Http2Connection>>(
      std::shared_ptr<Http2Connection>(new Http2Connection(std::move(impl))));
}
Result<std::shared_ptr<Http2Connection>> Http2Connection::AdoptTls(
    asio::ssl::stream<asio::ip::tcp::socket> socket, Http2Session::Role role,
    Http2StreamOptions options) {
  auto impl = std::make_shared<Impl>(
      Socket(std::move(socket)), role, "",
      std::chrono::steady_clock::time_point::max(), true);
  Status status = impl->Initialize(options);
  if (!status.ok()) return Result<std::shared_ptr<Http2Connection>>(status);
  return Result<std::shared_ptr<Http2Connection>>(
      std::shared_ptr<Http2Connection>(new Http2Connection(std::move(impl))));
}
asio::awaitable<Status> Http2Connection::Run() {
  auto impl = impl_;
  return DispatchConnection(impl, impl->Run());
}
asio::awaitable<StreamResult> Http2Connection::Open(
    Metadata headers, std::chrono::steady_clock::time_point deadline) {
  auto impl = impl_;
  return DispatchConnection(impl, impl->Open(std::move(headers), deadline));
}
asio::awaitable<StreamResult> Http2Connection::Accept() {
  auto impl = impl_;
  return DispatchConnection(impl, impl->Accept());
}
asio::awaitable<Status> Http2Connection::Shutdown(
    std::chrono::steady_clock::time_point deadline) {
  auto impl = impl_;
  return DispatchConnection(impl, impl->Shutdown(deadline));
}
asio::awaitable<Status> Http2Connection::Cancel() {
  auto impl = impl_;
  return DispatchConnection(impl, impl->Cancel(), false);
}
Http2ByteStream::Http2ByteStream(
    std::shared_ptr<Http2Connection::Impl> connection, int32_t id)
    : connection_(std::move(connection)), id_(id) {}
asio::any_io_executor Http2ByteStream::executor() const {
  return connection_->executor();
}
Http2ByteStream::~Http2ByteStream() {
  auto connection = std::move(connection_);
  auto executor = connection->executor();
  asio::post(executor, [connection = std::move(connection), id = id_] {
    connection->Release(id);
  });
}
asio::awaitable<Result<Metadata>> Http2ByteStream::ReadHeaders() {
  auto connection = connection_;
  return DispatchStream(connection, id_, connection->ReadHeaders(id_),
                        connection->client());
}
asio::awaitable<ByteResult> Http2ByteStream::Read() {
  auto connection = connection_;
  return DispatchStream(connection, id_, connection->Read(id_),
                        connection->client());
}
asio::awaitable<Status> Http2ByteStream::FinishSend() {
  auto connection = connection_;
  return DispatchStream(connection, id_, connection->FinishSend(id_));
}
asio::awaitable<Status> Http2ByteStream::DiscardRead() {
  auto connection = connection_;
  return DispatchStream(connection, id_, connection->DiscardRead(id_));
}
asio::awaitable<Result<Metadata>> Http2ByteStream::Finish() {
  auto connection = connection_;
  return DispatchStream(connection, id_, connection->Finish(id_));
}
asio::awaitable<Result<Metadata>> Http2ByteStream::ReadTrailers() {
  auto connection = connection_;
  return DispatchStream(connection, id_, connection->ReadTrailers(id_));
}
asio::awaitable<Status> Http2ByteStream::WaitClosed() {
  auto connection = connection_;
  return DispatchStream(connection, id_, connection->WaitClosed(id_), false);
}
asio::awaitable<Status> Http2ByteStream::Write(std::string bytes) {
  auto connection = connection_;
  return DispatchStream(connection, id_,
                        connection->Write(id_, std::move(bytes)));
}
asio::awaitable<Status> Http2ByteStream::SendHeaders(Metadata headers) {
  auto connection = connection_;
  return DispatchStream(connection, id_,
                        connection->SendHeaders(id_, std::move(headers)));
}
asio::awaitable<Status> Http2ByteStream::CloseSend(Metadata trailers) {
  auto connection = connection_;
  return DispatchStream(connection, id_,
                        connection->CloseSend(id_, std::move(trailers)));
}
asio::awaitable<Status> Http2ByteStream::SendResponse(HttpMessage response,
                                                      Metadata trailers) {
  auto connection = connection_;
  return DispatchStream(
      connection, id_,
      connection->SendResponse(id_, std::move(response), std::move(trailers)));
}
asio::awaitable<Status> Http2ByteStream::SetDeadline(
    std::chrono::steady_clock::time_point deadline) {
  auto connection = connection_;
  return DispatchStream(connection, id_,
                        connection->SetDeadline(id_, deadline));
}
asio::awaitable<Status> Http2ByteStream::Cancel() {
  auto connection = connection_;
  return DispatchStream(connection, id_, connection->CancelStream(id_));
}
}  // namespace connectrpc
