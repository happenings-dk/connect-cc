#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <map>
#include <memory>
#include <utility>

#include "connectrpc/http1_connection.h"
#include "connectrpc/http2_connection.h"

namespace connectrpc {
namespace {
asio::awaitable<void> InvokeStream(HttpStreamHandler handler,
                                   std::shared_ptr<ByteStream> stream) {
  co_await asio::this_coro::throw_if_cancelled(false);
  Status status = co_await handler(stream);
  if (!status.ok()) co_await stream->Cancel();
}
template <typename Connection>
asio::awaitable<Status> Serve(std::shared_ptr<Connection> connection,
                              HttpStreamHandler handler, size_t max_handlers) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = co_await asio::this_coro::executor;
  struct State {
    explicit State(asio::any_io_executor executor) : changed(executor, 1) {}
    asio::experimental::channel<void(asio::error_code)> changed;
    std::map<size_t, std::shared_ptr<asio::cancellation_signal>> tasks;
    bool run_done = false;
    Status outcome;
  };
  auto state = std::make_shared<State>(executor);
  asio::cancellation_signal stop_run;
  asio::co_spawn(
      executor, connection->Run(),
      asio::bind_cancellation_slot(stop_run.slot(), [state](auto exception,
                                                            Status status) {
        state->outcome =
            exception ? Status{Code::kInternal, "connection task failed", {}}
                      : std::move(status);
        state->run_done = true;
        for (const auto& [id, signal] : state->tasks)
          signal->emit(asio::cancellation_type::all);
        state->changed.try_send(asio::error_code{});
      }));
  size_t next_id = 0;
  bool canceled = false;
  Status accept_failure;
  while (!state->run_done) {
    const auto cancellation = co_await asio::this_coro::cancellation_state;
    if (cancellation.cancelled() != asio::cancellation_type::none) {
      canceled = true;
      break;
    }
    if (state->tasks.size() >= max_handlers) {
      auto [error] = co_await state->changed.async_receive(
          asio::as_tuple(asio::use_awaitable));
      if (error) {
        canceled = true;
        break;
      }
      continue;
    }
    auto accepted = co_await connection->Accept();
    if (!accepted.ok()) {
      const auto after_accept = co_await asio::this_coro::cancellation_state;
      canceled = after_accept.cancelled() != asio::cancellation_type::none;
      if (!canceled && accepted.status().code != Code::kUnavailable)
        accept_failure = accepted.status();
      break;
    }
    if (state->run_done) break;
    auto stream = *accepted.value();
    auto signal = std::make_shared<asio::cancellation_signal>();
    size_t id = next_id++;
    state->tasks.emplace(id, signal);
    asio::co_spawn(executor, InvokeStream(handler, stream),
                   asio::bind_cancellation_slot(
                       signal->slot(), [state, id, stream](auto exception) {
                         // Releasing the retained stream also cancels
                         // incomplete output.
                         (void)exception;
                         state->tasks.erase(id);
                         state->changed.try_send(asio::error_code{});
                       }));
  }
  bool abort_started = false;
  for (;;) {
    const auto cancellation = co_await asio::this_coro::cancellation_state;
    if (!abort_started &&
        cancellation.cancelled() != asio::cancellation_type::none)
      canceled = true;
    if (!abort_started && (canceled || !accept_failure.ok())) {
      abort_started = true;
      co_await asio::this_coro::reset_cancellation_state(
          asio::disable_cancellation());
      stop_run.emit(asio::cancellation_type::all);
      co_await connection->Cancel();
    }
    if (state->run_done && state->tasks.empty()) break;
    auto [error] = co_await state->changed.async_receive(
        asio::as_tuple(asio::use_awaitable));
    if (error) canceled = true;
  }
  if (canceled) co_return Status{Code::kCanceled, "server canceled", {}};
  if (!accept_failure.ok()) co_return accept_failure;
  co_return state->outcome;
}
}  // namespace
asio::awaitable<Status> ServeHttp1Connection(
    std::shared_ptr<Http1Connection> connection, HttpStreamHandler handler) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!connection || !handler)
    co_return Status{Code::kInvalidArgument, "invalid serving options", {}};
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());
    co_await connection->Cancel();
    co_return Status{Code::kCanceled, "server canceled", {}};
  }
  auto [exception, status] = co_await asio::co_spawn(
      asio::make_strand(co_await asio::this_coro::executor),
      Serve(std::move(connection), std::move(handler), 1),
      asio::as_tuple(asio::use_awaitable));
  if (exception) co_return Status{Code::kInternal, "server task failed", {}};
  co_return status;
}
asio::awaitable<Status> ServeHttp2Connection(
    std::shared_ptr<Http2Connection> connection, HttpStreamHandler handler,
    size_t max_handlers) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!connection || !handler || max_handlers == 0)
    co_return Status{Code::kInvalidArgument, "invalid serving options", {}};
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());
    co_await connection->Cancel();
    co_return Status{Code::kCanceled, "server canceled", {}};
  }
  auto [exception, status] = co_await asio::co_spawn(
      asio::make_strand(co_await asio::this_coro::executor),
      Serve(std::move(connection), std::move(handler), max_handlers),
      asio::as_tuple(asio::use_awaitable));
  if (exception) co_return Status{Code::kInternal, "server task failed", {}};
  co_return status;
}
}  // namespace connectrpc
