#include "connectrpc/interceptor.h"

#include <algorithm>
#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "connectrpc/http_connection.h"
#include "connectrpc/rpc.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
using Text = google::protobuf::StringValue;
struct Record {
  std::vector<std::string> events;
  std::vector<CallInfo> calls;
  std::vector<Code> completions;
  std::vector<std::pair<bool, size_t>> messages;
  Status reject;
  bool invalid_metadata = false;
  bool wait = false;
};
class Probe final : public CallInterceptor {
 public:
  Probe(Record& record, std::string name, const CallInfo& info)
      : record_(record), name_(std::move(name)), info_(info) {}
  asio::awaitable<Status> OnRequest(Metadata& headers) override {
    record_.events.push_back(name_ + ":request");
    headers.emplace_back(record_.invalid_metadata ? "content-type" : name_,
                         "request");
    if (record_.wait) {
      asio::steady_timer timer(co_await asio::this_coro::executor);
      timer.expires_after(std::chrono::seconds(5));
      auto [error] =
          co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
      record_.events.push_back(name_ + ":joined");
      if (error) co_return Status{Code::kCanceled, "auth interrupted", {}};
    }
    co_return record_.reject;
  }
  void OnResponseHeaders(Metadata& headers) override {
    record_.events.push_back(name_ + ":headers");
    headers.emplace_back(name_, "headers");
  }
  void OnResponseTrailers(Metadata& trailers) override {
    record_.events.push_back(name_ + ":trailers");
    trailers.emplace_back(name_, "trailers");
  }
  void OnMessage(bool outbound, size_t bytes) override {
    record_.messages.emplace_back(outbound, bytes);
  }
  void OnComplete(const Status& status) override {
    record_.events.push_back(name_ + ":complete");
    record_.completions.push_back(status.code);
    // The reference passed to the factory must still be valid here.
    record_.calls.push_back(info_);
  }

 private:
  Record& record_;
  std::string name_;
  const CallInfo& info_;
};
Interceptor Observe(Record& record, std::string name) {
  return [&record, name = std::move(name)](const CallInfo& info) {
    return std::make_unique<Probe>(record, name, info);
  };
}
bool Contains(const Metadata& metadata, std::string name, std::string value) {
  return std::find(metadata.begin(), metadata.end(),
                   std::pair(std::move(name), std::move(value))) !=
         metadata.end();
}
TEST(Interceptor, UnaryOrderingMetadataAndOperationLifetime) {
  for (bool get : {false, true}) {
    asio::io_context loop;
    Record client_record, server_record;
    Server server({Observe(server_record, "server-first"),
                   Observe(server_record, "server-second")});
    int called = 0;
    ASSERT_TRUE(
        (server.RegisterUnary<Text, Text>(
             "/test.Service/Echo",
             [&](Request<Text> request) -> asio::awaitable<Response<Text>> {
               ++called;
               for (const auto& name : {"client-first", "client-second",
                                        "server-first", "server-second"})
                 EXPECT_TRUE(Contains(request.headers, name, "request"));
               Response<Text> result;
               result.message = std::move(request.message);
               co_return result;
             },
             {Idempotency::kNoSideEffects}))
            .ok());
    auto client = std::make_unique<Client>(
        [&server](HttpMessage request,
                  TransportOptions) -> asio::awaitable<Result<HttpMessage>> {
          co_return Result<HttpMessage>(
              co_await server.Handle(std::move(request)));
        },
        StreamExchange{},
        std::vector<Interceptor>{Observe(client_record, "client-first"),
                                 Observe(client_record, "client-second")});
    Text input;
    input.set_value("hello");
    CallOptions options;
    options.use_get = get;
    auto operation = client->Unary<Text, Text>(
        "/test.Service/Echo", input, options, {Idempotency::kNoSideEffects});
    client.reset();
    int done = 0;
    asio::co_spawn(
        loop, std::move(operation), [&](auto exception, Response<Text> result) {
          EXPECT_FALSE(exception);
          EXPECT_TRUE(result.status.ok()) << result.status.message;
          ASSERT_TRUE(result.message);
          EXPECT_EQ(result.message->value(), "hello");
          for (const auto& name : {"client-first", "client-second",
                                   "server-first", "server-second"}) {
            EXPECT_TRUE(Contains(result.headers, name, "headers"));
            EXPECT_TRUE(Contains(result.trailers, name, "trailers"));
          }
          ++done;
        });
    loop.run();
    EXPECT_EQ(called, 1);
    EXPECT_EQ(done, 1);
    for (auto* record : {&client_record, &server_record}) {
      const std::string prefix = record == &client_record ? "client" : "server";
      EXPECT_EQ(record->events,
                (std::vector<std::string>{
                    prefix + "-first:request", prefix + "-second:request",
                    prefix + "-second:headers", prefix + "-first:headers",
                    prefix + "-second:trailers", prefix + "-first:trailers",
                    prefix + "-second:complete", prefix + "-first:complete"}));
      EXPECT_EQ(record->completions, (std::vector<Code>{Code::kOk, Code::kOk}));
      ASSERT_EQ(record->calls.size(), 2);
      EXPECT_EQ(record->calls[0].use_get, get);
      EXPECT_EQ(record->calls[0].client, record == &client_record);
      EXPECT_EQ(record->calls[0].procedure, "/test.Service/Echo");
      EXPECT_EQ(record->messages.size(), 4);
      for (const auto& [outbound, bytes] : record->messages)
        EXPECT_EQ(bytes, input.ByteSizeLong());
    }
  }
}
TEST(Interceptor, RejectionAndInvalidMetadataStopBeforeTransport) {
  for (bool invalid : {false, true}) {
    asio::io_context loop;
    Record first, second;
    first.invalid_metadata = invalid;
    if (!invalid) first.reject = {Code::kUnauthenticated, "no token", {}};
    int exchanges = 0;
    Client client(
        [&](HttpMessage,
            TransportOptions) -> asio::awaitable<Result<HttpMessage>> {
          ++exchanges;
          co_return Result<HttpMessage>(
              Status{Code::kInternal, "unexpected exchange", {}});
        },
        {}, {Observe(first, "first"), Observe(second, "second")});
    bool done = false;
    asio::co_spawn(
        loop, client.Unary<Text, Text>("/test/Echo", Text{}),
        [&](auto exception, Response<Text> result) {
          EXPECT_FALSE(exception);
          EXPECT_EQ(result.status.code,
                    invalid ? Code::kInvalidArgument : Code::kUnauthenticated);
          done = true;
        });
    loop.run();
    EXPECT_TRUE(done);
    EXPECT_EQ(exchanges, 0);
    EXPECT_EQ(first.completions.size(), 1);
    EXPECT_TRUE(second.events.empty());
  }
}
TEST(Interceptor, AwaitedAuthenticationIsCanceledAndJoined) {
  for (bool deadline : {false, true}) {
    asio::io_context loop;
    asio::cancellation_signal signal;
    Record record;
    record.wait = true;
    int exchanges = 0;
    Client client(
        [&](HttpMessage,
            TransportOptions) -> asio::awaitable<Result<HttpMessage>> {
          ++exchanges;
          co_return Result<HttpMessage>(
              Status{Code::kInternal, "unexpected exchange", {}});
        },
        {}, {Observe(record, "auth")});
    CallOptions options;
    if (deadline)
      options.transport.deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    bool done = false;
    asio::co_spawn(
        loop, client.Unary<Text, Text>("/test/Echo", Text{}, options),
        asio::bind_cancellation_slot(
            signal.slot(), [&](auto exception, Response<Text> result) {
              EXPECT_FALSE(exception);
              EXPECT_EQ(result.status.code,
                        deadline ? Code::kDeadlineExceeded : Code::kCanceled);
              EXPECT_NE(std::find(record.events.begin(), record.events.end(),
                                  "auth:joined"),
                        record.events.end());
              done = true;
            }));
    if (!deadline) {
      loop.poll();
      ASSERT_EQ(record.events, (std::vector<std::string>{"auth:request"}));
      signal.emit(asio::cancellation_type::all);
    }
    loop.run();
    EXPECT_TRUE(done);
    EXPECT_EQ(exchanges, 0);
    EXPECT_EQ(record.completions,
              (std::vector<Code>{deadline ? Code::kDeadlineExceeded
                                          : Code::kCanceled}));
  }
}
TEST(Interceptor, FactoriesCanOptOutAndInvalidFactoriesFail) {
  asio::io_context loop;
  bool done = false;
  auto operation = [&]() -> asio::awaitable<void> {
    auto empty = internal_interceptor::Call::Create({Interceptor{}}, {});
    EXPECT_EQ(empty.status().code, Code::kInvalidArgument);
    auto skipped = internal_interceptor::Call::Create(
        {[](const CallInfo&) -> std::unique_ptr<CallInterceptor> {
          return nullptr;
        }},
        {});
    EXPECT_TRUE(skipped.ok());
    auto headers = co_await (*skipped.value())->Start({});
    EXPECT_TRUE(headers.ok());
    auto repeated = co_await (*skipped.value())->Start({});
    EXPECT_EQ(repeated.status().code, Code::kFailedPrecondition);
    done = true;
  };
  asio::co_spawn(loop, operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  loop.run();
  EXPECT_TRUE(done);
}
}  // namespace
}  // namespace connectrpc

namespace connectrpc {
namespace {
void NetworkInterceptors(Protocol protocol, HttpVersion version, bool text,
                         bool deny) {
  SCOPED_TRACE(static_cast<int>(protocol));
  SCOPED_TRACE(static_cast<int>(version));
  SCOPED_TRACE(text);
  SCOPED_TRACE(deny);
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
  Record client_record, server_record;
  if (deny) server_record.reject = {Code::kUnauthenticated, "no token", {}};
  Server server({Observe(server_record, "server")});
  int handlers = 0;
  auto check = [&](const Metadata& headers) {
    ++handlers;
    EXPECT_TRUE(Contains(headers, "client", "request"));
    EXPECT_TRUE(Contains(headers, "server", "request"));
  };
  ASSERT_TRUE(
      (server.RegisterUnary<Text, Text>(
           "/test/Echo",
           [&](Request<Text> request) -> asio::awaitable<Response<Text>> {
             check(request.headers);
             Response<Text> response;
             response.message = std::move(request.message);
             co_return response;
           }))
          .ok());
  ASSERT_TRUE((server.RegisterServerStream<Text, Text>(
                   "/test/Expand",
                   [&](Request<Text> request, TypedStream<Text, Text> stream)
                       -> asio::awaitable<StreamCompletion> {
                     check(request.headers);
                     Status status = co_await stream.Write(request.message);
                     co_return StreamCompletion{status, {}};
                   }))
                  .ok());
  ASSERT_TRUE((server.RegisterClientStream<Text, Text>(
                   "/test/Collect",
                   [&](CallContext context, TypedStream<Text, Text> stream)
                       -> asio::awaitable<Response<Text>> {
                     check(context.headers);
                     Response<Text> response;
                     for (;;) {
                       auto next = co_await stream.Read();
                       if (!next.ok()) {
                         response.status = next.status();
                         break;
                       }
                       if (!*next.value()) break;
                       response.message = std::move(**next.value());
                     }
                     co_return response;
                   }))
                  .ok());
  ASSERT_TRUE((server.RegisterBidiStream<Text, Text>(
                   "/test/Chat",
                   [&](CallContext context, TypedStream<Text, Text> stream)
                       -> asio::awaitable<StreamCompletion> {
                     check(context.headers);
                     for (;;) {
                       auto next = co_await stream.Read();
                       if (!next.ok())
                         co_return StreamCompletion{next.status(), {}};
                       if (!*next.value()) break;
                       auto status = co_await stream.Write(**next.value());
                       if (!status.ok()) co_return StreamCompletion{status, {}};
                     }
                     co_return StreamCompletion{};
                   }))
                  .ok());
  Client client({},
                [client_transport](Metadata headers, TransportOptions options)
                    -> asio::awaitable<Result<std::shared_ptr<ByteStream>>> {
                  headers.emplace(headers.begin(), ":scheme", "http");
                  headers.emplace(headers.begin(), ":authority", "localhost");
                  co_return co_await client_transport->Open(std::move(headers),
                                                            options.deadline);
                },
                {Observe(client_record, "client")});
  const int calls = version == HttpVersion::kHttp1 ? 3 : 4;
  int done = 0, connections_done = 0;
  asio::steady_timer watchdog(loop);
  watchdog.expires_after(std::chrono::seconds(5));
  watchdog.async_wait([&](asio::error_code error) {
    if (!error) {
      ADD_FAILURE() << "intercepted calls stalled";
      loop.stop();
    }
  });
  for (auto connection : {client_transport, server_transport})
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
  auto serve = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < calls; ++i) {
      auto stream = co_await server_transport->Accept();
      EXPECT_TRUE(stream.ok());
      if (!stream.ok()) co_return;
      auto status = co_await server.HandleHttpStream(*stream.value());
      EXPECT_TRUE(status.ok()) << status.message;
    }
  };
  auto call = [&]() -> asio::awaitable<void> {
    Text input;
    input.set_value("hello");
    CallOptions unary_options;
    unary_options.protocol = protocol;
    unary_options.grpc_web_text = text;
    auto response =
        co_await client.Unary<Text, Text>("/test/Echo", input, unary_options);
    const Code expected = deny ? Code::kUnauthenticated : Code::kOk;
    EXPECT_EQ(response.status.code, expected) << response.status.message;
    EXPECT_TRUE(Contains(response.headers, "server", "headers"));
    EXPECT_TRUE(Contains(response.headers, "client", "headers"));
    EXPECT_TRUE(Contains(response.trailers, "server", "trailers"));
    EXPECT_TRUE(Contains(response.trailers, "client", "trailers"));
    ++done;
    for (int shape = 1; shape < calls; ++shape) {
      StreamCallOptions options;
      options.protocol = protocol;
      options.grpc_web_text = text;
      Result<TypedStream<Text, Text>> opened;
      if (shape == 1)
        opened = co_await client.ServerStream<Text, Text>("/test/Expand", input,
                                                          options);
      else if (shape == 2)
        opened =
            co_await client.ClientStream<Text, Text>("/test/Collect", options);
      else
        opened = co_await client.BidiStream<Text, Text>("/test/Chat", options);
      EXPECT_TRUE(opened.ok()) << opened.status().message;
      if (!opened.ok()) co_return;
      EXPECT_EQ(client_record.completions.size(), shape);
      auto stream = *opened.value();
      if (shape != 1) {
        auto written = co_await stream.Write(input);
        EXPECT_TRUE(written.ok()) << written.message;
        EXPECT_TRUE((co_await stream.CloseSend()).ok());
      }
      auto headers = co_await stream.ReadHeaders();
      EXPECT_TRUE(headers.ok()) << headers.status().message;
      if (headers.ok()) {
        EXPECT_TRUE(Contains(*headers.value(), "server", "headers"));
        EXPECT_TRUE(Contains(*headers.value(), "client", "headers"));
      }
      int received = 0;
      for (;;) {
        auto next = co_await stream.Read();
        EXPECT_TRUE(next.ok()) << next.status().message;
        if (!next.ok() || !*next.value()) break;
        EXPECT_EQ((**next.value()).value(), "hello");
        ++received;
      }
      EXPECT_EQ(received, deny ? 0 : 1);
      EXPECT_EQ(client_record.completions.size(), shape);
      auto completion = co_await stream.Finish();
      EXPECT_EQ(completion.status.code, expected) << completion.status.message;
      EXPECT_TRUE(Contains(completion.trailers, "server", "trailers"));
      EXPECT_TRUE(Contains(completion.trailers, "client", "trailers"));
      EXPECT_EQ(client_record.completions.size(), shape + 1);
      EXPECT_EQ((co_await stream.Finish()).status.code, expected);
      co_await stream.Cancel();
      EXPECT_EQ(client_record.completions.size(), shape + 1);
      ++done;
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
  EXPECT_EQ(done, calls);
  EXPECT_EQ(handlers, deny ? 0 : calls);
  EXPECT_EQ(connections_done, 2);
  for (auto* record : {&client_record, &server_record}) {
    std::vector<std::string> expected_events;
    const std::string name = record == &client_record ? "client" : "server";
    for (int i = 0; i < calls; ++i) {
      for (const auto& event : {"request", "headers", "trailers", "complete"})
        expected_events.push_back(name + ":" + event);
    }
    EXPECT_EQ(record->events, expected_events);
    EXPECT_EQ(
        record->completions,
        std::vector<Code>(calls, deny ? Code::kUnauthenticated : Code::kOk));
    EXPECT_EQ(record->calls.size(), calls);
    for (const auto& info : record->calls) {
      EXPECT_EQ(info.client, record == &client_record);
      EXPECT_EQ(info.protocol, protocol);
    }
    EXPECT_EQ(record->messages.size(),
              deny ? (record == &client_record ? calls : 0) : calls * 2);
  }
}
TEST(Interceptor, NetworkMetadataAndCompletionAcrossAllProtocolsAndShapes) {
  for (bool deny : {false, true}) {
    for (Protocol protocol :
         {Protocol::kConnect, Protocol::kGrpc, Protocol::kGrpcWeb})
      NetworkInterceptors(protocol, HttpVersion::kHttp2, false, deny);
    NetworkInterceptors(Protocol::kGrpcWeb, HttpVersion::kHttp2, true, deny);
    NetworkInterceptors(Protocol::kConnect, HttpVersion::kHttp1, false, deny);
    NetworkInterceptors(Protocol::kGrpcWeb, HttpVersion::kHttp1, false, deny);
    NetworkInterceptors(Protocol::kGrpcWeb, HttpVersion::kHttp1, true, deny);
  }
}
TEST(Interceptor, AbandonedCallCompletesEnteredHooksOnce) {
  asio::io_context loop;
  Record record;
  auto operation = [&]() -> asio::awaitable<void> {
    auto created =
        internal_interceptor::Call::Create({Observe(record, "call")}, {});
    EXPECT_TRUE(created.ok());
    auto headers = co_await (*created.value())->Start({});
    EXPECT_TRUE(headers.ok());
    EXPECT_TRUE(record.completions.empty());
  };
  asio::co_spawn(loop, operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  loop.run();
  EXPECT_EQ(record.completions, (std::vector<Code>{Code::kCanceled}));
}
}  // namespace
}  // namespace connectrpc

namespace connectrpc {
namespace {
TEST(Interceptor, ServerAuthenticationDeadlineJoinsBeforeCompletion) {
  asio::io_context loop;
  Record record;
  record.wait = true;
  Server server({Observe(record, "auth")});
  int called = 0;
  ASSERT_TRUE((server.RegisterUnary<Text, Text>(
                   "/test/Echo",
                   [&](Request<Text>) -> asio::awaitable<Response<Text>> {
                     ++called;
                     co_return Response<Text>{};
                   }))
                  .ok());
  CallOptions options;
  options.transport.deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
  auto request = EncodeUnaryRequest("/test/Echo", "", options);
  ASSERT_TRUE(request.ok());
  bool done = false;
  asio::co_spawn(loop, server.Handle(std::move(*request.value())),
                 [&](auto exception, HttpMessage response) {
                   EXPECT_FALSE(exception);
                   auto decoded = DecodeUnaryResponse(std::move(response),
                                                      Codec::kProtobuf, 1024);
                   EXPECT_EQ(decoded.status.code, Code::kDeadlineExceeded);
                   EXPECT_TRUE(Contains(decoded.headers, "auth", "headers"));
                   EXPECT_TRUE(Contains(decoded.trailers, "auth", "trailers"));
                   EXPECT_EQ(record.events,
                             (std::vector<std::string>{
                                 "auth:request", "auth:joined", "auth:headers",
                                 "auth:trailers", "auth:complete"}));
                   done = true;
                 });
  loop.run();
  EXPECT_TRUE(done);
  EXPECT_EQ(called, 0);
  EXPECT_EQ(record.completions, (std::vector<Code>{Code::kDeadlineExceeded}));
}
TEST(Interceptor, StreamingAuthenticationCancellationDoesNotOpenTransport) {
  asio::io_context loop;
  asio::cancellation_signal signal;
  Record record;
  record.wait = true;
  int exchanges = 0;
  Client client({},
                [&](Metadata, TransportOptions)
                    -> asio::awaitable<Result<std::shared_ptr<ByteStream>>> {
                  ++exchanges;
                  co_return Result<std::shared_ptr<ByteStream>>(
                      Status{Code::kInternal, "unexpected exchange", {}});
                },
                {Observe(record, "auth")});
  bool done = false;
  asio::co_spawn(
      loop, client.BidiStream<Text, Text>("/test/Chat"),
      asio::bind_cancellation_slot(
          signal.slot(),
          [&](auto exception, Result<TypedStream<Text, Text>> result) {
            EXPECT_FALSE(exception);
            EXPECT_EQ(result.status().code, Code::kCanceled);
            done = true;
          }));
  loop.poll();
  ASSERT_EQ(record.events, (std::vector<std::string>{"auth:request"}));
  signal.emit(asio::cancellation_type::all);
  loop.run();
  EXPECT_TRUE(done);
  EXPECT_EQ(exchanges, 0);
  EXPECT_EQ(record.events,
            (std::vector<std::string>{"auth:request", "auth:joined",
                                      "auth:complete"}));
  EXPECT_EQ(record.completions, (std::vector<Code>{Code::kCanceled}));
}
}  // namespace
}  // namespace connectrpc
