#include "connectrpc/transport.h"

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "gtest/gtest.h"
namespace connectrpc {
namespace {
TEST(Transport, UnaryLoopback) {
  asio::io_context loop;
  asio::error_code error;
  asio::ip::tcp::acceptor acceptor(loop);
  acceptor.open(asio::ip::tcp::v4(), error);
  ASSERT_FALSE(error);
  acceptor.bind({asio::ip::address_v4::loopback(), 0}, error);
  ASSERT_FALSE(error);
  acceptor.listen(1, error);
  ASSERT_FALSE(error);
  auto endpoint = acceptor.local_endpoint(error);
  ASSERT_FALSE(error);
  asio::ip::tcp::socket client(loop);
  client.connect(endpoint, error);
  ASSERT_FALSE(error);
  asio::ip::tcp::socket server(loop);
  acceptor.accept(server, error);
  ASSERT_FALSE(error);
  bool client_done = false;
  bool server_done = false;
  asio::steady_timer watchdog(loop);
  watchdog.expires_after(std::chrono::seconds(5));
  watchdog.async_wait([&loop](asio::error_code timer_error) {
    if (!timer_error) {
      ADD_FAILURE() << "transport timed out";
      loop.stop();
    }
  });
  asio::co_spawn(loop,
                 ServeHttp2(std::move(server),
                            [](HttpMessage request) {
                              return HttpMessage{
                                  0,
                                  {{":status", "200"},
                                   {"content-type", "application/proto"}},
                                  request.body};
                            }),
                 [&server_done, &watchdog](auto exception, Status status) {
                   EXPECT_FALSE(exception);
                   EXPECT_TRUE(status.ok()) << status.message;
                   server_done = true;
                   watchdog.cancel();
                 });
  asio::co_spawn(loop,
                 UnaryHttp2(std::move(client),
                            {{":method", "POST"},
                             {":scheme", "http"},
                             {":authority", "localhost"},
                             {":path", "/test.Service/Echo"},
                             {"content-type", "application/proto"}},
                            std::string(100000, 'x')),
                 [&client_done](auto exception, Result<HttpMessage> result) {
                   EXPECT_FALSE(exception);
                   ASSERT_TRUE(result.ok()) << result.status().message;
                   EXPECT_EQ(result.value()->body, std::string(100000, 'x'));
                   client_done = true;
                 });
  loop.run();
  EXPECT_TRUE(client_done);
  EXPECT_TRUE(server_done);
}
// A single client connection must make progress on a fast request while a
// previous handler is asleep. Resetting the slow request cancels and joins it.
TEST(Transport, ConcurrentHandlersAndResetCancellation) {
  asio::io_context loop;
  asio::error_code error;
  asio::ip::tcp::acceptor acceptor(loop);
  acceptor.open(asio::ip::tcp::v4(), error);
  ASSERT_FALSE(error);
  acceptor.bind({asio::ip::address_v4::loopback(), 0}, error);
  ASSERT_FALSE(error);
  acceptor.listen(1, error);
  ASSERT_FALSE(error);
  auto endpoint = acceptor.local_endpoint(error);
  ASSERT_FALSE(error);
  asio::ip::tcp::socket client(loop);
  client.connect(endpoint, error);
  ASSERT_FALSE(error);
  asio::ip::tcp::socket accepted(loop);
  acceptor.accept(accepted, error);
  ASSERT_FALSE(error);
  bool slow_started = false;
  bool slow_canceled = false;
  bool fast_done = false;
  bool server_done = false;
  asio::steady_timer watchdog(loop);
  watchdog.expires_after(std::chrono::seconds(3));
  watchdog.async_wait([&loop](asio::error_code error) {
    if (!error) {
      ADD_FAILURE() << "multiplexed calls stalled";
      loop.stop();
    }
  });
  UnaryHandler handler =
      [&slow_started,
       &slow_canceled](HttpMessage request) -> asio::awaitable<HttpMessage> {
    co_await asio::this_coro::throw_if_cancelled(false);
    if (request.body == "slow") {
      slow_started = true;
      asio::steady_timer timer(co_await asio::this_coro::executor);
      timer.expires_after(std::chrono::seconds(10));
      auto [error] =
          co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
      slow_canceled = error == asio::error::operation_aborted;
    }
    co_return HttpMessage{0, {{":status", "200"}}, request.body};
  };
  asio::co_spawn(loop, ServeHttp2(std::move(accepted), handler),
                 [&](auto exception, Status status) {
                   EXPECT_FALSE(exception);
                   EXPECT_TRUE(status.ok()) << status.message;
                   EXPECT_TRUE(slow_canceled);
                   server_done = true;
                   watchdog.cancel();
                 });
  auto operation = [&]() -> asio::awaitable<void> {
    auto session = Http2Session::Create(Http2Session::Role::kClient, 1024, 1024,
                                        [&](HttpMessage response) {
                                          EXPECT_EQ(response.body, "fast");
                                          fast_done = true;
                                        });
    EXPECT_TRUE(session.ok());
    if (!session.ok()) co_return;
    Metadata headers = {{":method", "POST"},
                        {":scheme", "http"},
                        {":authority", "localhost"},
                        {":path", "/echo"}};
    auto slow = (*session.value())->Request(headers, "slow");
    auto fast = (*session.value())->Request(headers, "fast");
    EXPECT_TRUE(slow.ok());
    EXPECT_TRUE(fast.ok());
    if (!slow.ok() || !fast.ok()) co_return;
    for (;;) {
      auto bytes = (*session.value())->Drain();
      EXPECT_TRUE(bytes.ok());
      if (!bytes.ok()) co_return;
      if (!bytes.value()->empty()) {
        auto [write_error, count] =
            co_await asio::async_write(client, asio::buffer(*bytes.value()),
                                       asio::as_tuple(asio::use_awaitable));
        (void)count;
        EXPECT_FALSE(write_error);
        if (write_error) co_return;
      }
      if (fast_done) break;
      char buffer[4096];
      auto [read_error, count] = co_await client.async_read_some(
          asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
      EXPECT_FALSE(read_error);
      if (read_error) co_return;
      EXPECT_TRUE(
          (*session.value())->Receive(std::string_view(buffer, count)).ok());
      if (fast_done) EXPECT_TRUE((*session.value())->Reset(*slow.value()).ok());
    }
    EXPECT_TRUE(slow_started);
    // Wait for the server to process RST_STREAM before closing the connection.
    asio::steady_timer settle(co_await asio::this_coro::executor);
    settle.expires_after(std::chrono::milliseconds(20));
    auto [settle_error] =
        co_await settle.async_wait(asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(settle_error);
    EXPECT_TRUE(slow_canceled);
    client.close(error);
  };
  asio::co_spawn(loop, operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  loop.run();
  EXPECT_TRUE(fast_done);
  EXPECT_TRUE(server_done);
}
void CheckStalledPeer(bool cancel) {
  asio::io_context loop;
  asio::error_code error;
  asio::ip::tcp::acceptor acceptor(loop);
  acceptor.open(asio::ip::tcp::v4(), error);
  ASSERT_FALSE(error);
  acceptor.bind({asio::ip::address_v4::loopback(), 0}, error);
  ASSERT_FALSE(error);
  acceptor.listen(1, error);
  ASSERT_FALSE(error);
  auto endpoint = acceptor.local_endpoint(error);
  ASSERT_FALSE(error);
  asio::ip::tcp::socket client(loop);
  client.connect(endpoint, error);
  ASSERT_FALSE(error);
  asio::ip::tcp::socket server(loop);
  acceptor.accept(server, error);
  ASSERT_FALSE(error);
  bool done = false;
  asio::cancellation_signal cancellation;
  asio::steady_timer timer(loop);
  if (cancel) {
    timer.expires_after(std::chrono::milliseconds(20));
    timer.async_wait([&cancellation](asio::error_code error) {
      if (!error) cancellation.emit(asio::cancellation_type::terminal);
    });
  }
  TransportOptions options;
  options.deadline = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(cancel ? 3000 : 20);
  asio::co_spawn(
      loop,
      UnaryHttp2(std::move(client),
                 {{":method", "POST"},
                  {":scheme", "http"},
                  {":authority", "localhost"},
                  {":path", "/echo"}},
                 "", options),
      asio::bind_cancellation_slot(
          cancellation.slot(),
          [&done, cancel](auto exception, Result<HttpMessage> result) {
            EXPECT_FALSE(exception);
            EXPECT_EQ(result.status().code,
                      cancel ? Code::kCanceled : Code::kDeadlineExceeded);
            done = true;
          }));
  loop.run();
  EXPECT_TRUE(done);
}
TEST(Transport, DeadlineCancelsStalledPeer) { CheckStalledPeer(false); }
TEST(Transport, CallerCancelsStalledPeer) { CheckStalledPeer(true); }
}  // namespace
}  // namespace connectrpc
