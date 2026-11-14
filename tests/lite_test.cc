#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>

#include "gtest/gtest.h"
#include "test/v1/lite.connect.h"

namespace connectrpc {
namespace {
using Message = lite::v1::Message;
using Stream = TypedStream<Message, Message>;
class LiteService final : public generated::lite::v1::EchoHandler {
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
TEST(Lite, GeneratedServiceRoundTripWithoutReflection) {
  asio::io_context loop;
  Server server;
  LiteService handler;
  ASSERT_TRUE(generated::lite::v1::RegisterEchoHandler(server, handler).ok());
  Client runtime(
      [&](HttpMessage request,
          TransportOptions options) -> asio::awaitable<Result<HttpMessage>> {
        co_return Result<HttpMessage>(
            co_await server.Handle(std::move(request), options.max_body_bytes));
      });
  generated::lite::v1::EchoClient client(runtime);
  int completed = 0;
  auto invoke = [&]() -> asio::awaitable<void> {
    for (Codec codec : {Codec::kProtobuf}) {
      Message input;
      input.set_value("Editions");
      (*input.mutable_counts())["count"] = 7;

      CallOptions options;
      options.codec = codec;
      auto response = co_await client.Say(input, options);
      EXPECT_TRUE(response.status.ok()) << response.status.message;
      if (response.message) {
        EXPECT_EQ(response.message->counts().at("count"), 7);
        EXPECT_EQ(response.message->value(), "Editions");

        ++completed;
      }
    }
  };
  asio::co_spawn(loop, invoke(), [](auto error) { EXPECT_FALSE(error); });
  loop.run();
  EXPECT_EQ(completed, 1);
}
TEST(Lite, JsonFailsExplicitlyAndDeterministicBinaryWorks) {
  Message message;
  (*message.mutable_counts())["z"] = 1;
  (*message.mutable_counts())["a"] = 2;
  auto encoded = EncodeDeterministicMessage(message, Codec::kProtobuf);
  ASSERT_TRUE(encoded.ok());
  Message decoded;
  EXPECT_TRUE(DecodeMessage(*encoded.value(), Codec::kProtobuf, decoded).ok());
  auto again = EncodeDeterministicMessage(decoded, Codec::kProtobuf);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(*encoded.value(), *again.value());
  EXPECT_EQ(EncodeMessage(message, Codec::kJson).status().code,
            Code::kUnimplemented);
  EXPECT_EQ(DecodeMessage("{}", Codec::kJson, decoded).code,
            Code::kUnimplemented);
}
}  // namespace

}  // namespace connectrpc
