#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>

#include "gtest/gtest.h"
#include "test/v1/edition.connect.h"

namespace connectrpc {
namespace {
using Message = edition::v1::Outer_Nested;
using Stream = TypedStream<Message, Message>;
class EditionService final : public generated::edition::v1::EchoHandler {
 public:
  asio::awaitable<Response<Message>> Say(Request<Message> request) override {
    Response<Message> response;
    response.message = std::move(request.message);
    co_return response;
  }
  asio::awaitable<StreamCompletion> Expand(Request<Message> request,
                                           Stream stream) override {
    co_return StreamCompletion{co_await stream.Write(request.message), {}};
  }
  asio::awaitable<Response<Message>> Collect(CallContext,
                                             Stream stream) override {
    Response<Message> response;
    auto next = co_await stream.Read();
    response.status = next.status();
    if (next.ok()) response.message = std::move(*next.value());
    co_return response;
  }
  asio::awaitable<StreamCompletion> Chat(CallContext, Stream stream) override {
    for (;;) {
      auto next = co_await stream.Read();
      if (!next.ok()) co_return StreamCompletion{next.status(), {}};
      if (!*next.value()) co_return StreamCompletion{};
      auto status = co_await stream.Write(**next.value());
      if (!status.ok()) co_return StreamCompletion{status, {}};
    }
  }
};
TEST(Editions, Imported2023MessageWith2024ServicePreservesFeatures) {
  asio::io_context loop;
  Server server;
  EditionService handler;
  ASSERT_TRUE(
      generated::edition::v1::RegisterEchoHandler(server, handler).ok());
  Client runtime(
      [&](HttpMessage request,
          TransportOptions options) -> asio::awaitable<Result<HttpMessage>> {
        co_return Result<HttpMessage>(
            co_await server.Handle(std::move(request), options.max_body_bytes));
      });
  generated::edition::v1::EchoClient client(runtime);
  int completed = 0;
  auto invoke = [&]() -> asio::awaitable<void> {
    for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
      Message input;
      input.set_value("Editions");
      input.set_count(0);
      input.add_numbers(7);
      input.add_numbers(8);
      CallOptions options;
      options.codec = codec;
      auto response = co_await client.Say(input, options);
      EXPECT_TRUE(response.status.ok()) << response.status.message;
      if (response.message) {
        EXPECT_TRUE(response.message->has_count());
        EXPECT_EQ(response.message->value(), "Editions");
        EXPECT_EQ(response.message->numbers_size(), 2);
        EXPECT_EQ(response.message->numbers(1), 8);
        ++completed;
      }
    }
  };
  asio::co_spawn(loop, invoke(), [](auto error) { EXPECT_FALSE(error); });
  loop.run();
  EXPECT_EQ(completed, 2);
}
}  // namespace
}  // namespace connectrpc
