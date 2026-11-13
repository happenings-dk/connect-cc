#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <functional>
#include <memory>
#include <utility>

#include "connectrpc/callback.h"
#include "connectrpc/http2_connection.h"
#include "connectrpc/http_connection.h"
#include "gtest/gtest.h"
#include "test/v1/echo.connect.h"

namespace connectrpc {
namespace {
class EchoService final : public generated::test::v1::EchoHandler {
 public:
  asio::awaitable<Response<shared::v1::Outer_Nested>> Say(
      Request<shared::v1::Outer_Nested> request) override {
    Response<shared::v1::Outer_Nested> response;
    response.message = std::move(request.message);
    co_return response;
  }
  asio::awaitable<Response<shared::v1::class_>> class_(
      Request<shared::v1::class_> request) override {
    Response<shared::v1::class_> response;
    response.message = std::move(request.message);
    co_return response;
  }
  asio::awaitable<Response<shared::v1::class_>> class_Rpc(
      Request<shared::v1::class_> request) override {
    return class_(std::move(request));
  }
  asio::awaitable<Response<shared::v1::Outer_Nested>> EchoClientRpc(
      Request<shared::v1::Outer_Nested> request) override {
    return Say(std::move(request));
  }
};
TEST(Generated, ImportedNestedMessagesKeywordsAndCollisions) {
  asio::io_context loop;
  Server server;
  EchoService handler;
  ASSERT_TRUE(generated::test::v1::RegisterEchoHandler(server, handler).ok());
  Client runtime([&server](HttpMessage request, TransportOptions options)
                     -> asio::awaitable<Result<HttpMessage>> {
    co_return Result<HttpMessage>(
        co_await server.Handle(std::move(request), options.max_body_bytes));
  });
  generated::test::v1::EchoClient client(runtime);
  shared::v1::Outer_Nested input;
  input.set_value("generated call");
  input.set_count(42);
  int completed = 0;
  Async(loop.get_executor(), client.Say(input),
        [&completed](Response<shared::v1::Outer_Nested> response) {
          ASSERT_TRUE(response.status.ok()) << response.status.message;
          ASSERT_TRUE(response.message);
          EXPECT_EQ(response.message->value(), "generated call");
          EXPECT_TRUE(response.message->has_count());
          EXPECT_EQ(response.message->count(), 42);
          ++completed;
        });
  shared::v1::class_ keyword;
  keyword.set_value("keyword method");
  asio::co_spawn(
      loop, client.class_Rpc(keyword),
      [&completed](auto exception, Response<shared::v1::class_> response) {
        EXPECT_FALSE(exception);
        ASSERT_TRUE(response.status.ok()) << response.status.message;
        ASSERT_TRUE(response.message);
        EXPECT_EQ(response.message->value(), "keyword method");
        ++completed;
      });
  loop.run();
  EXPECT_EQ(completed, 2);
}
TEST(Generated, GetUsesSchemaEligibility) {
  asio::io_context loop;
  Server server;
  EchoService handler;
  ASSERT_TRUE(generated::test::v1::RegisterEchoHandler(server, handler).ok());
  int exchanges = 0;
  Client runtime(
      [&](HttpMessage request,
          TransportOptions options) -> asio::awaitable<Result<HttpMessage>> {
        ++exchanges;
        EXPECT_TRUE(request.body.empty());
        bool get = false;
        for (const auto& [name, value] : request.headers)
          if (name == ":method") get = value == "GET";
        EXPECT_TRUE(get);
        co_return Result<HttpMessage>(
            co_await server.Handle(std::move(request), options.max_body_bytes));
      });
  generated::test::v1::EchoClient client(runtime);
  bool completed = false;
  auto invoke = [&]() -> asio::awaitable<void> {
    for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
      for (bool gzip : {false, true}) {
        CallOptions options;
        options.use_get = true;
        options.codec = codec;
        options.compress = gzip;
        shared::v1::Outer_Nested input;
        input.set_value("GET & + / 你好");
        input.set_count(73);
        auto result = co_await client.Say(input, options);
        EXPECT_TRUE(result.status.ok()) << result.status.message;
        if (result.message) {
          EXPECT_EQ(result.message->value(), input.value());
          EXPECT_EQ(result.message->count(), 73);
        }
        // This method has no NO_SIDE_EFFECTS option: fail before transport.
        auto denied = co_await client.EchoClientRpc(input, options);
        EXPECT_EQ(denied.status.code, Code::kInvalidArgument);
        options.protocol = Protocol::kGrpcWeb;
        denied = co_await client.Say(input, options);
        EXPECT_EQ(denied.status.code, Code::kInvalidArgument);
      }
    }
    completed = true;
  };
  asio::co_spawn(loop, invoke(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  loop.run();
  EXPECT_TRUE(completed);
  EXPECT_EQ(exchanges, 4);
}
TEST(Generated, DuplicateRegistrationDoesNotPartiallyRegisterService) {
  Server server;
  EchoService handler;
  ASSERT_TRUE((server.RegisterUnary<shared::v1::class_, shared::v1::class_>(
                   "/test.v1.Echo/class",
                   [&handler](Request<shared::v1::class_> request) {
                     return handler.class_(std::move(request));
                   }))
                  .ok());
  EXPECT_EQ(generated::test::v1::RegisterEchoHandler(server, handler).code,
            Code::kAlreadyExists);
  EXPECT_TRUE(server.CanRegister("/test.v1.Echo/Say").ok());
}
using StreamInput = shared::v1::Outer_Nested;
using StreamOutput = shared::v1::class_;
using HandlerStream = TypedStream<StreamOutput, StreamInput>;
class StreamingService final : public generated::test::v1::StreamingHandler {
 public:
  asio::awaitable<StreamCompletion> Expand(Request<StreamInput> request,
                                           HandlerStream stream) override {
    StreamOutput output;
    output.set_value(request.message.value());
    for (int i = 0; i < 2; ++i) {
      Status sent = co_await stream.Write(output);
      if (!sent.ok()) co_return StreamCompletion{sent, {}};
    }
    co_return StreamCompletion{{}, {{"shape", "server"}}};
  }
  asio::awaitable<Response<StreamOutput>> Collect(
      CallContext context, HandlerStream stream) override {
    EXPECT_EQ(context.headers, (Metadata{{"generated", "yes"}}));
    Response<StreamOutput> response;
    response.message.emplace();
    for (;;) {
      auto next = co_await stream.Read();
      if (!next.ok()) {
        response.status = next.status();
        co_return response;
      }
      if (!*next.value()) break;
      response.message->mutable_value()->append((**next.value()).value());
    }
    response.trailers = {{"shape", "client"}};
    co_return response;
  }
  asio::awaitable<StreamCompletion> Chat(CallContext context,
                                         HandlerStream stream) override {
    EXPECT_EQ(context.headers, (Metadata{{"generated", "yes"}}));
    for (;;) {
      auto next = co_await stream.Read();
      if (!next.ok()) co_return StreamCompletion{next.status(), {}};
      if (!*next.value()) break;
      StreamOutput output;
      output.set_value((**next.value()).value());
      Status sent = co_await stream.Write(output);
      if (!sent.ok()) co_return StreamCompletion{sent, {}};
    }
    co_return StreamCompletion{{}, {{"shape", "bidi"}}};
  }
};
// Retained by pending callbacks; no coroutine is used to consume this stream.
class CallbackStream : public std::enable_shared_from_this<CallbackStream> {
 public:
  CallbackStream(asio::any_io_executor executor,
                 TypedStream<StreamInput, StreamOutput> stream, int shape,
                 std::function<void()> done)
      : executor_(std::move(executor)),
        stream_(std::move(stream)),
        shape_(shape),
        done_(std::move(done)) {}
  CallbackStream(const CallbackStream&) = delete;
  CallbackStream& operator=(const CallbackStream&) = delete;
  void Start() {
    if (shape_ == 0)
      Read();
    else
      Write();
  }

 private:
  void Done() {
    if (done_) std::exchange(done_, {})();
  }
  void Write() {
    StreamInput input;
    input.set_value("hello");
    Async(executor_, stream_.Write(input),
          [self = shared_from_this()](Status status) {
            EXPECT_TRUE(status.ok()) << status.message;
            if (!status.ok()) {
              self->Done();
              return;
            }
            if (++self->written_ < 2) {
              self->Write();
              return;
            }
            Async(self->executor_, self->stream_.CloseSend(),
                  [self](Status status) {
                    EXPECT_TRUE(status.ok()) << status.message;
                    if (status.ok())
                      self->Read();
                    else
                      self->Done();
                  });
          });
  }
  void Read() {
    Async(
        executor_, stream_.Read(),
        [self = shared_from_this()](Result<std::optional<StreamOutput>> next) {
          EXPECT_TRUE(next.ok()) << next.status().message;
          if (!next.ok()) {
            self->Done();
            return;
          }
          if (*next.value()) {
            EXPECT_EQ((**next.value()).value(),
                      self->shape_ == 1 ? "hellohello" : "hello");
            ++self->received_;
            self->Read();
            return;
          }
          EXPECT_EQ(self->received_, self->shape_ == 1 ? 1 : 2);
          Async(self->executor_, self->stream_.Finish(),
                [self](StreamCompletion result) {
                  EXPECT_TRUE(result.status.ok()) << result.status.message;
                  EXPECT_EQ(
                      result.trailers,
                      (Metadata{{"shape", self->shape_ == 0   ? "server"
                                          : self->shape_ == 1 ? "client"
                                                              : "bidi"}}));
                  self->Done();
                });
        });
  }
  asio::any_io_executor executor_;
  TypedStream<StreamInput, StreamOutput> stream_;
  int shape_;
  std::function<void()> done_;
  int written_ = 0;
  int received_ = 0;
};
void GeneratedStreamingCalls(bool callbacks,
                             Protocol protocol = Protocol::kConnect,
                             bool text = false,
                             HttpVersion version = HttpVersion::kHttp2) {
  const int shape_count = version == HttpVersion::kHttp1 ? 2 : 3;
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
  asio::ip::tcp::socket client_socket(loop), server_socket(loop);
  client_socket.connect(endpoint, error);
  ASSERT_FALSE(error);
  acceptor.accept(server_socket, error);
  ASSERT_FALSE(error);
  auto client_connection = HttpConnection::Create(
      std::move(client_socket), HttpConnection::Role::kClient, version);
  auto server_connection = HttpConnection::Create(
      std::move(server_socket), HttpConnection::Role::kServer, version);
  ASSERT_TRUE(client_connection.ok());
  ASSERT_TRUE(server_connection.ok());
  auto client_transport = *client_connection.value();
  auto server_transport = *server_connection.value();
  Server server;
  StreamingService handler;
  ASSERT_TRUE(
      generated::test::v1::RegisterStreamingHandler(server, handler).ok());
  Client runtime(
      {},
      [client_transport](Metadata headers, TransportOptions options)
          -> asio::awaitable<Result<std::shared_ptr<ByteStream>>> {
        headers.emplace(headers.begin(), ":scheme", "http");
        headers.emplace(headers.begin(), ":authority", "localhost");
        auto stream = co_await client_transport->Open(std::move(headers),
                                                      options.deadline);
        if (!stream.ok())
          co_return Result<std::shared_ptr<ByteStream>>(stream.status());
        co_return Result<std::shared_ptr<ByteStream>>(*stream.value());
      });
  generated::test::v1::StreamingClient client(runtime);
  int completed = 0;
  int connections_done = 0;
  asio::steady_timer watchdog(loop);
  watchdog.expires_after(std::chrono::seconds(5));
  watchdog.async_wait([&](asio::error_code error) {
    if (!error) {
      ADD_FAILURE() << "generated streaming calls stalled";
      loop.stop();
    }
  });
  for (auto connection : {client_transport, server_transport}) {
    asio::co_spawn(
        loop, connection->Run(),
        [&, connection](auto exception, Status status) {
          EXPECT_FALSE(exception);
          EXPECT_TRUE(status.ok() || (version == HttpVersion::kHttp1 &&
                                      connection == server_transport &&
                                      status.code == Code::kUnavailable))
              << status.message;
          if (++connections_done == 2) watchdog.cancel();
        });
  }
  auto serve = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < shape_count; ++i) {
      auto accepted = co_await server_transport->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      Status status = co_await server.HandleStream(*accepted.value());
      EXPECT_TRUE(status.ok()) << status.message;
    }
  };
  auto call = [&]() -> asio::awaitable<void> {
    for (int shape = 0; shape < shape_count; ++shape) {
      StreamInput input;
      input.set_value("hello");
      StreamCallOptions options;
      options.headers = {{"generated", "yes"}};
      options.protocol = protocol;
      options.grpc_web_text = text;
      if (callbacks) {
        asio::experimental::channel<void(asio::error_code)> done(loop, 1);
        auto on_open =
            [&](Result<TypedStream<StreamInput, StreamOutput>> result) {
              EXPECT_TRUE(result.ok()) << result.status().message;
              if (!result.ok()) {
                done.try_send(asio::error_code{});
                return;
              }
              std::make_shared<CallbackStream>(
                  loop.get_executor(), std::move(*result.value()), shape,
                  [&] { EXPECT_TRUE(done.try_send(asio::error_code{})); })
                  ->Start();
            };
        if (shape == 0)
          Async(loop.get_executor(), client.Expand(input, options), on_open);
        else if (shape == 1)
          Async(loop.get_executor(), client.Collect(options), on_open);
        else
          Async(loop.get_executor(), client.Chat(options), on_open);
        auto [error] =
            co_await done.async_receive(asio::as_tuple(asio::use_awaitable));
        EXPECT_FALSE(error);
        ++completed;
        continue;
      }
      Result<TypedStream<StreamInput, StreamOutput>> opened;
      if (shape == 0)
        opened = co_await client.Expand(input, options);
      else if (shape == 1)
        opened = co_await client.Collect(options);
      else
        opened = co_await client.Chat(options);
      EXPECT_TRUE(opened.ok()) << opened.status().message;
      if (!opened.ok()) co_return;
      auto stream = *opened.value();
      if (shape != 0) {
        EXPECT_TRUE((co_await stream.Write(input)).ok());
        EXPECT_TRUE((co_await stream.Write(input)).ok());
        EXPECT_TRUE((co_await stream.CloseSend()).ok());
      }
      int count = 0;
      for (;;) {
        auto next = co_await stream.Read();
        EXPECT_TRUE(next.ok()) << next.status().message;
        if (!next.ok() || !*next.value()) break;
        EXPECT_EQ((**next.value()).value(),
                  shape == 1 ? "hellohello" : "hello");
        ++count;
      }
      EXPECT_EQ(count, shape == 1 ? 1 : 2);
      auto result = co_await stream.Finish();
      EXPECT_TRUE(result.status.ok()) << result.status.message;
      EXPECT_EQ(result.trailers, (Metadata{{"shape", shape == 0   ? "server"
                                                     : shape == 1 ? "client"
                                                                  : "bidi"}}));
      ++completed;
    }
    EXPECT_TRUE(
        (co_await client_transport->Shutdown(std::chrono::steady_clock::now() +
                                             std::chrono::seconds(1)))
            .ok());
  };
  asio::co_spawn(loop, serve(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop, call(), [](auto exception) { EXPECT_FALSE(exception); });
  loop.run();
  EXPECT_EQ(completed, shape_count);
  EXPECT_EQ(connections_done, 2);
}
TEST(Generated, StreamingBindingsRouteImportedNestedMessages) {
  GeneratedStreamingCalls(false);
}
TEST(Generated, CallbackStreamingBindings) {
  for (Protocol protocol :
       {Protocol::kConnect, Protocol::kGrpc, Protocol::kGrpcWeb})
    GeneratedStreamingCalls(true, protocol);
  GeneratedStreamingCalls(true, Protocol::kGrpcWeb, true);
  GeneratedStreamingCalls(true, Protocol::kConnect, false, HttpVersion::kHttp1);
  GeneratedStreamingCalls(true, Protocol::kGrpcWeb, false, HttpVersion::kHttp1);
  GeneratedStreamingCalls(true, Protocol::kGrpcWeb, true, HttpVersion::kHttp1);
}
}  // namespace
}  // namespace connectrpc
