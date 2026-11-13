#include "connectrpc/callback.h"

#include <asio/as_tuple.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/bind_executor.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
template <typename T>
asio::awaitable<T> Ready(T value) {
  co_return std::move(value);
}
asio::awaitable<Status> Delayed(asio::steady_timer& timer, bool& started) {
  co_await asio::this_coro::throw_if_cancelled(false);
  started = true;
  auto [error] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
  co_return error ? Status{Code::kCanceled, "timer canceled", {}} : Status{};
}
TEST(Callback, DeferredCompletionAndMoveOnlyValues) {
  asio::io_context loop;
  int completed = 0;
  auto owned = std::make_unique<int>(7);
  Async(loop.get_executor(),
        Ready(Result<std::unique_ptr<int>>(std::move(owned))),
        [capture = std::make_unique<int>(9),
         &completed](Result<std::unique_ptr<int>> result) {
          ASSERT_TRUE(result.ok());
          EXPECT_EQ(**result.value(), 7);
          EXPECT_EQ(*capture, 9);
          ++completed;
        });
  Async(loop.get_executor(), Ready(Status{Code::kNotFound, "missing", {}}),
        [&](Status status) {
          EXPECT_EQ(status.code, Code::kNotFound);
          ++completed;
        });
  EXPECT_EQ(completed, 0);
  loop.run();
  EXPECT_EQ(completed, 2);
}
struct Allocations {
  std::atomic<int> allocated = 0;
  std::atomic<int> freed = 0;
};
template <typename T>
struct CountingAllocator {
  using value_type = T;
  std::shared_ptr<Allocations> counts;
  explicit CountingAllocator(std::shared_ptr<Allocations> c)
      : counts(std::move(c)) {}
  template <typename U>
  CountingAllocator(const CountingAllocator<U>& other) : counts(other.counts) {}
  T* allocate(size_t n) {
    counts->allocated.fetch_add(1);
    return std::allocator<T>{}.allocate(n);
  }
  void deallocate(T* pointer, size_t n) {
    counts->freed.fetch_add(1);
    std::allocator<T>{}.deallocate(pointer, n);
  }
  template <typename U>
  bool operator==(const CountingAllocator<U>& other) const {
    return counts == other.counts;
  }
};
TEST(Callback, AssociatedExecutorAllocatorAndLateCancellation) {
  asio::io_context operations;
  asio::io_context completions;
  asio::cancellation_signal signal;
  auto counts = std::make_shared<Allocations>();
  bool ran = false;
  bool done = false;
  auto operation = [&]() -> asio::awaitable<Status> {
    EXPECT_TRUE(operations.get_executor().running_in_this_thread());
    EXPECT_FALSE(completions.get_executor().running_in_this_thread());
    ran = true;
    co_return Status{};
  };
  Async(operations.get_executor(), operation(),
        asio::bind_executor(
            completions.get_executor(),
            asio::bind_allocator(
                CountingAllocator<char>(counts),
                asio::bind_cancellation_slot(signal.slot(), [&](Status status) {
                  EXPECT_TRUE(status.ok());
                  EXPECT_TRUE(
                      completions.get_executor().running_in_this_thread());
                  EXPECT_FALSE(
                      operations.get_executor().running_in_this_thread());
                  done = true;
                }))));
  operations.run();
  EXPECT_TRUE(ran);
  EXPECT_FALSE(done);
  // The successful result is already queued on the completion executor.
  signal.emit(asio::cancellation_type::all);
  completions.run();
  EXPECT_TRUE(done);
  operations.restart();
  operations.run();
  // Releasing the caller's slot releases the cancellation marker allocation.
  signal.slot().clear();
  EXPECT_GT(counts->allocated.load(), 0);
  EXPECT_EQ(counts->allocated.load(), counts->freed.load());
}
TEST(Callback, CancellationBeforeStartupAndDuringWait) {
  for (bool before_start : {false, true}) {
    asio::io_context loop;
    asio::cancellation_signal signal;
    asio::steady_timer timer(loop);
    timer.expires_after(std::chrono::seconds(2));
    bool started = false;
    int done = 0;
    Async(loop.get_executor(), Delayed(timer, started),
          asio::bind_cancellation_slot(signal.slot(), [&](Status status) {
            EXPECT_EQ(status.code, Code::kCanceled);
            ++done;
          }));
    if (!before_start) {
      EXPECT_GT(loop.poll(), 0);
      EXPECT_TRUE(started);
    }
    signal.emit(asio::cancellation_type::all);
    loop.run();
    EXPECT_EQ(started, !before_start);
    EXPECT_EQ(done, 1);
  }
}
TEST(Callback, PartialCancellationDoesNotPromiseRpcRollback) {
  asio::io_context loop;
  asio::cancellation_signal signal;
  asio::steady_timer timer(loop);
  timer.expires_after(std::chrono::milliseconds(1));
  bool started = false;
  bool done = false;
  Async(loop.get_executor(), Delayed(timer, started),
        asio::bind_cancellation_slot(signal.slot(), [&](Status status) {
          EXPECT_TRUE(status.ok());
          done = true;
        }));
  signal.emit(asio::cancellation_type::partial |
              asio::cancellation_type::total);
  loop.run();
  EXPECT_TRUE(started);
  EXPECT_TRUE(done);
}
TEST(Callback, SeparateExecutorThreadsAndCancellation) {
  asio::io_context operations;
  asio::io_context completions;
  asio::cancellation_signal signal;
  asio::steady_timer timer(operations);
  timer.expires_after(std::chrono::seconds(2));
  std::atomic<bool> started = false;
  bool done = false;
  auto operation = [&]() -> asio::awaitable<Status> {
    co_await asio::this_coro::throw_if_cancelled(false);
    started.store(true);
    started.notify_one();
    auto [error] =
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    co_return error ? Status{Code::kCanceled, "timer canceled", {}} : Status{};
  };
  Async(operations.get_executor(), operation(),
        asio::bind_executor(
            completions.get_executor(),
            asio::bind_cancellation_slot(signal.slot(), [&](Status status) {
              EXPECT_TRUE(completions.get_executor().running_in_this_thread());
              EXPECT_FALSE(operations.get_executor().running_in_this_thread());
              EXPECT_EQ(status.code, Code::kCanceled);
              done = true;
            })));
  std::thread operation_thread([&] { operations.run(); });
  std::thread completion_thread([&] { completions.run(); });
  started.wait(false);
  signal.emit(asio::cancellation_type::all);
  operation_thread.join();
  completion_thread.join();
  EXPECT_TRUE(done);
}
TEST(Callback, UnexpectedAsioFailureBecomesStatus) {
  asio::io_context loop;
  asio::steady_timer timer(loop);
  timer.expires_after(std::chrono::seconds(2));
  auto operation = [&]() -> asio::awaitable<StreamCompletion> {
    // Asio's default token raises on a canceled timer. The adapter's co_spawn
    // boundary reports this unexpected coroutine failure as a value.
    co_await timer.async_wait(asio::use_awaitable);
    co_return StreamCompletion{};
  };
  bool done = false;
  Async(loop.get_executor(), operation(), [&](StreamCompletion result) {
    EXPECT_EQ(result.status.code, Code::kInternal);
    done = true;
  });
  loop.poll();
  timer.cancel();
  loop.run();
  EXPECT_TRUE(done);
}
TEST(Callback, LazyTokensAndInlineInitiation) {
  asio::io_context loop;
  bool started = false;
  int completed = 0;
  auto operation = [&]() -> asio::awaitable<Status> {
    started = true;
    co_return Status{};
  };
  auto lazy = Async(loop.get_executor(), operation(), asio::deferred);
  EXPECT_EQ(loop.poll(), 0);
  EXPECT_FALSE(started);
  loop.restart();
  std::move(lazy)([&](Status status) {
    EXPECT_TRUE(status.ok());
    ++completed;
  });
  auto invoke = [&]() -> asio::awaitable<void> {
    auto initiating = std::make_shared<bool>(true);
    Async(loop.get_executor(), Ready(Status{}),
          [&completed, initiating](Status status) {
            EXPECT_FALSE(*initiating);
            EXPECT_TRUE(status.ok());
            ++completed;
          });
    *initiating = false;
    auto result = co_await Async(loop.get_executor(), Ready(Status{}),
                                 asio::use_awaitable);
    EXPECT_TRUE(result.ok());
    ++completed;
  };
  asio::co_spawn(loop, invoke(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  loop.run();
  EXPECT_TRUE(started);
  EXPECT_EQ(completed, 3);
}
TEST(Callback, UnaryResultsMatchCoroutineAndClientLifetime) {
  using Message = google::protobuf::StringValue;
  asio::io_context loop;
  int calls = 0;
  Server server;
  ASSERT_TRUE(
      (server.RegisterUnary<Message, Message>(
           "/test.Service/Echo",
           [&](Request<Message> request) -> asio::awaitable<Response<Message>> {
             ++calls;
             co_return Response<Message>{{},
                                         std::move(request.message),
                                         {{"reply", "yes"}},
                                         {{"done", "yes"}}};
           }))
          .ok());
  UnaryExchange exchange =
      [&](HttpMessage request,
          TransportOptions options) -> asio::awaitable<Result<HttpMessage>> {
    co_return Result<HttpMessage>(
        co_await server.Handle(std::move(request), options.max_body_bytes));
  };
  Response<Message> callback_result;
  Response<Message> coroutine_result;
  Message input;
  input.set_value("callback parity");
  {
    Client client(exchange);
    Async(
        loop.get_executor(),
        client.Unary<Message, Message>("/test.Service/Echo", input),
        [&](Response<Message> result) { callback_result = std::move(result); });
    asio::co_spawn(loop,
                   client.Unary<Message, Message>("/test.Service/Echo", input),
                   [&](auto exception, Response<Message> result) {
                     EXPECT_FALSE(exception);
                     coroutine_result = std::move(result);
                   });
  }
  loop.run();
  ASSERT_TRUE(callback_result.status.ok());
  ASSERT_TRUE(coroutine_result.status.ok());
  ASSERT_TRUE(callback_result.message);
  ASSERT_TRUE(coroutine_result.message);
  EXPECT_EQ(callback_result.message->value(),
            coroutine_result.message->value());
  EXPECT_EQ(callback_result.headers, coroutine_result.headers);
  EXPECT_EQ(callback_result.trailers, coroutine_result.trailers);
  EXPECT_EQ(calls, 2);
}
}  // namespace
}  // namespace connectrpc
