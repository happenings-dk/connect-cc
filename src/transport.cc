#include "connectrpc/transport.h"

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <map>
#include <optional>
#include <type_traits>
#include <utility>

#include "connectrpc/tls.h"
namespace connectrpc {
namespace {
Status IoError(const asio::error_code& error) {
  return {error == asio::error::operation_aborted ? Code::kCanceled
                                                  : Code::kUnavailable,
          error.message(),
          {}};
}
template <typename Stream>
asio::awaitable<Status> Flush(Http2Session& session, Stream& socket) {
  for (;;) {
    const auto cancellation = co_await asio::this_coro::cancellation_state;
    if (cancellation.cancelled() != asio::cancellation_type::none)
      co_return Status{Code::kCanceled, "call canceled", {}};
    auto bytes = session.Drain();
    if (!bytes.ok()) co_return bytes.status();
    if (bytes.value()->empty()) co_return Status{};
    auto [error, count] =
        co_await asio::async_write(socket, asio::buffer(*bytes.value()),
                                   asio::as_tuple(asio::use_awaitable));
    (void)count;
    if (error) co_return IoError(error);
  }
}
// Timer and socket continuations run on the call's strand. The shared state
// remains alive for the queued timer completion after the guard is destroyed.
class DeadlineGuard {
 public:
  DeadlineGuard(asio::any_io_executor executor,
                asio::ip::tcp::socket::lowest_layer_type& socket,
                std::chrono::steady_clock::time_point deadline)
      : state_(std::make_shared<State>()), timer_(executor) {
    state_->socket = &socket;
    if (deadline == std::chrono::steady_clock::time_point::max()) return;
    timer_.expires_at(deadline);
    std::shared_ptr<State> state = state_;
    timer_.async_wait([state](asio::error_code error) {
      if (error || !state->socket) return;
      state->expired = true;
      asio::error_code ignored;
      state->socket->cancel(ignored);
    });
  }
  ~DeadlineGuard() { state_->socket = nullptr; }
  bool expired() const { return state_->expired; }

 private:
  struct State {
    asio::ip::tcp::socket::lowest_layer_type* socket = nullptr;
    bool expired = false;
  };
  std::shared_ptr<State> state_;
  asio::steady_timer timer_;
};
// All access runs on the connection strand. Separate reader and writer tasks
// let slow RPC handlers yield without delaying unrelated streams or SETTINGS.
template <typename Stream, typename Handler>
struct ServerConnection {
  ServerConnection(Stream stream, Handler callback,
                   asio::any_io_executor executor)
      : socket(std::move(stream)),
        handler(std::move(callback)),
        wake(executor, 1),
        joined(executor, 1) {}
  Stream socket;
  Handler handler;
  std::unique_ptr<Http2Session> session;
  asio::experimental::channel<void(asio::error_code)> wake;
  asio::experimental::channel<void(asio::error_code)> joined;
  std::map<int32_t, std::shared_ptr<asio::cancellation_signal>> calls;
  size_t tasks = 0;
  bool stopping = false;
  Status failure;
  void Wake() { wake.try_send(asio::error_code{}); }
  void Completed() {
    --tasks;
    joined.try_send(asio::error_code{});
  }
  void Stop(Status status) {
    if (stopping) return;
    stopping = true;
    failure = std::move(status);
    asio::error_code ignored;
    socket.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_send,
                                   ignored);
    socket.lowest_layer().close(ignored);
    wake.close();
    for (const auto& [id, signal] : calls)
      signal->emit(asio::cancellation_type::all);
  }
};
template <typename State>
asio::awaitable<Status> WriteServer(std::shared_ptr<State> state) {
  co_await asio::this_coro::throw_if_cancelled(false);
  while (!state->stopping) {
    Status status = co_await Flush(*state->session, state->socket);
    if (!status.ok()) co_return status;
    auto [error] =
        co_await state->wake.async_receive(asio::as_tuple(asio::use_awaitable));
    if (error) break;
  }
  co_return Status{};
}
template <typename State>
asio::awaitable<HttpMessage> DispatchServer(std::shared_ptr<State> state,
                                            HttpMessage request) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if constexpr (std::is_same_v<
                    std::invoke_result_t<decltype(state->handler), HttpMessage>,
                    HttpMessage>)
    co_return state->handler(std::move(request));
  else
    co_return co_await state->handler(std::move(request));
}
template <typename Stream, typename Handler>
asio::awaitable<Status> RunServeHttp2(Stream socket, Handler handler) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = co_await asio::this_coro::executor;
  using State = ServerConnection<Stream, Handler>;
  auto state =
      std::make_shared<State>(std::move(socket), std::move(handler), executor);
  // Session callbacks cannot own the state, which owns the session.
  std::weak_ptr<State> weak = state;
  auto session = Http2Session::Create(
      Http2Session::Role::kServer, 4 * 1024 * 1024, 16 * 1024,
      [weak, executor](HttpMessage request) {
        auto state = weak.lock();
        if (!state || state->stopping) return;
        int32_t id = request.stream_id;
        // Count canceled-but-not-yet-completed handlers too: an uncooperative
        // handler must not permit an unbounded number of replacement tasks.
        if (state->tasks >= 101) {
          state->session->Reset(id, 7);  // REFUSED_STREAM
          state->Wake();
          return;
        }
        auto signal = std::make_shared<asio::cancellation_signal>();
        state->calls.emplace(id, signal);
        ++state->tasks;
        asio::co_spawn(
            executor, DispatchServer(state, std::move(request)),
            asio::bind_cancellation_slot(
                signal->slot(),
                [state, signal, id](auto exception, HttpMessage response) {
                  bool active = state->calls.erase(id) != 0;
                  if (active && !state->stopping) {
                    Status status = exception
                                        ? Status{Code::kInternal,
                                                 "unexpected handler failure",
                                                 {}}
                                        : state->session->Respond(
                                              id, std::move(response.headers),
                                              std::move(response.body));
                    if (!status.ok())
                      state->session->Reset(id, 2);  // INTERNAL_ERROR
                    state->Wake();
                  }
                  state->Completed();
                }));
      },
      [weak](int32_t id, Status) {
        auto state = weak.lock();
        if (!state) return;
        auto found = state->calls.find(id);
        if (found == state->calls.end()) return;
        auto signal = found->second;
        state->calls.erase(found);
        signal->emit(asio::cancellation_type::all);
      });
  if (!session.ok()) co_return session.status();
  state->session = std::move(*session.value());
  ++state->tasks;
  asio::co_spawn(
      executor, WriteServer(state), [state](auto exception, Status status) {
        if (exception)
          status = {Code::kInternal, "unexpected writer failure", {}};
        if (!status.ok()) state->Stop(std::move(status));
        state->Completed();
      });
  while (!state->stopping) {
    char buffer[16384];
    auto [error, count] = co_await state->socket.async_read_some(
        asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
    if (error) {
      state->Stop(error == asio::error::eof ? Status{} : IoError(error));
      break;
    }
    Status status = state->session->Receive(std::string_view(buffer, count));
    if (!status.ok()) {
      state->Stop(std::move(status));
      break;
    }
    state->Wake();
  }
  // Join every child before releasing the operation's handler or socket.
  // Disable inherited cancellation here so cancellation cannot bypass the join.
  co_await asio::this_coro::reset_cancellation_state(
      asio::disable_cancellation());
  while (state->tasks) {
    auto [error] = co_await state->joined.async_receive(
        asio::as_tuple(asio::use_awaitable));
    if (error) break;
  }
  co_return state->failure;
}
}  // namespace
asio::awaitable<Status> ServeHttp2(asio::ip::tcp::socket socket,
                                   UnaryHandler handler) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = asio::make_strand(socket.get_executor());
  auto [error, result] = co_await asio::co_spawn(
      executor, RunServeHttp2(std::move(socket), std::move(handler)),
      asio::as_tuple(asio::use_awaitable));
  co_return error ? Status{Code::kInternal, "unexpected coroutine failure", {}}
                  : result;
}
asio::awaitable<Status> ServeHttp2(
    asio::ip::tcp::socket socket,
    std::function<HttpMessage(HttpMessage)> handler) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = asio::make_strand(socket.get_executor());
  auto [error, result] = co_await asio::co_spawn(
      executor, RunServeHttp2(std::move(socket), std::move(handler)),
      asio::as_tuple(asio::use_awaitable));
  co_return error ? Status{Code::kInternal, "unexpected coroutine failure", {}}
                  : result;
}
namespace {
asio::awaitable<Status> Handshake(
    asio::ssl::stream<asio::ip::tcp::socket>& stream, bool server,
    std::chrono::steady_clock::time_point deadline_time) {
  if (deadline_time <= std::chrono::steady_clock::now()) {
    co_return Status{Code::kDeadlineExceeded, "TLS deadline exceeded", {}};
  }
  DeadlineGuard deadline(co_await asio::this_coro::executor,
                         stream.lowest_layer(), deadline_time);
  auto [error] = co_await stream.async_handshake(
      server ? asio::ssl::stream_base::server : asio::ssl::stream_base::client,
      asio::as_tuple(asio::use_awaitable));
  if (deadline.expired())
    co_return Status{Code::kDeadlineExceeded, "TLS deadline exceeded", {}};
  if (error) co_return IoError(error);
  co_return CheckHttp2Alpn(stream);
}
template <typename Handler>
asio::awaitable<Status> RunTlsServer(
    asio::ssl::stream<asio::ip::tcp::socket> stream, Handler handler,
    std::chrono::steady_clock::time_point handshake_deadline) {
  co_await asio::this_coro::throw_if_cancelled(false);
  Status status = co_await Handshake(stream, true, handshake_deadline);
  if (!status.ok()) co_return status;
  co_return co_await RunServeHttp2(std::move(stream), std::move(handler));
}
}  // namespace
asio::awaitable<Status> ServeHttp2Tls(
    asio::ssl::stream<asio::ip::tcp::socket> stream,
    std::function<HttpMessage(HttpMessage)> handler,
    std::chrono::steady_clock::time_point handshake_deadline) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = asio::make_strand(stream.get_executor());
  auto [error, result] = co_await asio::co_spawn(
      executor,
      RunTlsServer(std::move(stream), std::move(handler), handshake_deadline),
      asio::as_tuple(asio::use_awaitable));
  co_return error ? Status{Code::kInternal, "unexpected coroutine failure", {}}
                  : result;
}
asio::awaitable<Status> ServeHttp2Tls(
    asio::ssl::stream<asio::ip::tcp::socket> stream, UnaryHandler handler,
    std::chrono::steady_clock::time_point handshake_deadline) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = asio::make_strand(stream.get_executor());
  auto [error, result] = co_await asio::co_spawn(
      executor,
      RunTlsServer(std::move(stream), std::move(handler), handshake_deadline),
      asio::as_tuple(asio::use_awaitable));
  co_return error ? Status{Code::kInternal, "unexpected coroutine failure", {}}
                  : result;
}
}  // namespace connectrpc
