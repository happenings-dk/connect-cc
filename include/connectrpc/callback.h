#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_CALLBACK_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_CALLBACK_H_

#include <asio/any_io_executor.hpp>
#include <asio/associated_allocator.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/bind_executor.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#include "connectrpc/rpc.h"
#include "connectrpc/status.h"
#include "connectrpc/streaming_protocol.h"

namespace connectrpc {
// Runs an RPC/stream/transport coroutine on executor and completes with
// void(T). Supports Status, Result<T>, Response<T>, and StreamCompletion. Plain
// callbacks receive the same result as co_await; no exception argument is
// exposed. Preserves the token's associated executor, allocator and
// cancellation slot. Completion is never inline within initiation. With lazy
// tokens (for example use_awaitable or deferred), execution starts when the
// token is initiated. Terminal cancellation is supported, matching co_spawn and
// RPC operations. Keep the bound cancellation signal alive until completion;
// use one slot per active operation. User callbacks must not throw. The
// application owns and runs the executors; no thread or hidden event loop is
// created.
template <typename T, typename CompletionToken>
auto Async(asio::any_io_executor executor, asio::awaitable<T> operation,
           CompletionToken&& token);

// Implementation details only below here.
namespace internal_callback {
template <typename T>
struct Failure;
template <>
struct Failure<Status> {
  static Status Make(Status status) { return status; }
};
template <typename T>
struct Failure<Result<T>> {
  static Result<T> Make(Status status) { return Result<T>(std::move(status)); }
};
template <typename T>
struct Failure<Response<T>> {
  static Response<T> Make(Status status) {
    return {std::move(status), std::nullopt, {}, {}};
  }
};
template <>
struct Failure<StreamCompletion> {
  static StreamCompletion Make(Status status) {
    return {std::move(status), {}};
  }
};
template <typename T>
asio::awaitable<T> Run(asio::awaitable<T> operation,
                       std::shared_ptr<std::atomic<bool>> canceled) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (canceled->load(std::memory_order_relaxed))
    co_return Failure<T>::Make({Code::kCanceled, "operation canceled", {}});
  co_return co_await std::move(operation);
}
class Initiate {
 public:
  using executor_type = asio::any_io_executor;
  explicit Initiate(executor_type executor) : executor_(std::move(executor)) {}
  Initiate(const Initiate&) = default;
  Initiate& operator=(const Initiate&) = default;
  executor_type get_executor() const { return executor_; }

  template <typename Handler, typename T>
  void operator()(Handler handler, asio::awaitable<T> operation) const {
    auto completion_executor =
        asio::get_associated_executor(handler, executor_);
    auto allocator = asio::get_associated_allocator(handler);
    auto slot = asio::get_associated_cancellation_slot(handler);
    // co_spawn can report an exception if canceled before coroutine startup.
    // Record that signal independently of the caller-owned slot's state. The
    // completion may run on another executor, so the marker is atomic.
    auto canceled = std::allocate_shared<std::atomic<bool>>(allocator, false);
    asio::cancellation_state cancellation(
        slot,
        [canceled](asio::cancellation_type type) {
          auto accepted = type & asio::cancellation_type::terminal;
          if (accepted != asio::cancellation_type::none)
            canceled->store(true, std::memory_order_relaxed);
          return accepted;
        },
        asio::enable_terminal_cancellation());
    auto complete = [handler = std::move(handler), canceled](auto exception,
                                                             T result) mutable {
      if (exception) {
        result = Failure<T>::Make(
            canceled->load(std::memory_order_relaxed)
                ? Status{Code::kCanceled, "operation canceled", {}}
                : Status{Code::kInternal, "unexpected coroutine failure", {}});
      }
      // A completed result wins a later cancellation. In particular, preserve
      // returned stream handles so the caller can finish or cancel them.
      std::move(handler)(std::move(result));
    };
    asio::co_spawn(
        executor_, Run(std::move(operation), canceled),
        asio::bind_executor(
            completion_executor,
            asio::bind_allocator(
                allocator, asio::bind_cancellation_slot(cancellation.slot(),
                                                        std::move(complete)))));
  }

 private:
  executor_type executor_;
};
}  // namespace internal_callback
template <typename T, typename CompletionToken>
auto Async(asio::any_io_executor executor, asio::awaitable<T> operation,
           CompletionToken&& token) {
  return asio::async_initiate<CompletionToken, void(T)>(
      internal_callback::Initiate(std::move(executor)), token,
      std::move(operation));
}
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_CALLBACK_H_
