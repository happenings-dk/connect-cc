#include "connectrpc/unary.h"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <vector>

#include "connectrpc/rpc.h"
#include "connectrpc/streaming_protocol.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
using Text = google::protobuf::StringValue;
TEST(Compression, ConnectPreferenceIncludesPostGetAndStreaming) {
  struct Case {
    const char* accept;
    bool gzip;
  };
  const Case cases[] = {{"identity, gzip", false},
                        {"gzip, identity", true},
                        {"br, identity, gzip", false},
                        {"br, gzip, identity", true},
                        {" identity , gzip ", false},
                        {"gzip", true},
                        {"br", false},
                        {"identity", false}};
  for (bool compressed : {false, true}) {
    for (const auto& test : cases) {
      SCOPED_TRACE(test.accept);
      for (bool get : {false, true}) {
        CallOptions options;
        options.compress = compressed;
        options.use_get = get;
        auto wire = EncodeUnaryRequest("/test.Service/Echo", "payload", options,
                                       {Idempotency::kNoSideEffects});
        ASSERT_TRUE(wire.ok());
        for (auto& [name, value] : wire.value()->headers)
          if (name == "accept-encoding") value = test.accept;
        auto request = DecodeUnaryRequest(*wire.value(), 1024);
        ASSERT_TRUE(request.ok()) << request.status().message;
        EXPECT_EQ(request.value()->accept_gzip, test.gzip);
        std::erase_if(wire.value()->headers, [](const auto& field) {
          return field.first == "accept-encoding";
        });
        request = DecodeUnaryRequest(*wire.value(), 1024);
        ASSERT_TRUE(request.ok());
        EXPECT_EQ(request.value()->accept_gzip, compressed);
      }
      StreamingRequest input;
      input.procedure = "/test.Service/Echo";
      input.gzip = compressed;
      auto headers = EncodeStreamingRequest(input);
      ASSERT_TRUE(headers.ok());
      for (auto& [name, value] : *headers.value())
        if (name == "connect-accept-encoding") value = test.accept;
      auto request = DecodeStreamingRequest(*headers.value());
      ASSERT_TRUE(request.ok());
      EXPECT_EQ(request.value()->accept_gzip, test.gzip);
      std::erase_if(*headers.value(), [](const auto& field) {
        return field.first == "connect-accept-encoding";
      });
      request = DecodeStreamingRequest(*headers.value());
      ASSERT_TRUE(request.ok());
      EXPECT_EQ(request.value()->accept_gzip, compressed);
    }
  }
}
TEST(Compression, GrpcAcceptEncodingRemainsACapabilityList) {
  for (Protocol protocol : {Protocol::kGrpc, Protocol::kGrpcWeb}) {
    StreamingRequest input;
    input.procedure = "/test.Service/Echo";
    input.protocol = protocol;
    auto headers = EncodeStreamingRequest(input);
    ASSERT_TRUE(headers.ok());
    for (auto& [name, value] : *headers.value())
      if (name == "grpc-accept-encoding") value = "identity, gzip";
    auto request = DecodeStreamingRequest(*headers.value());
    ASSERT_TRUE(request.ok());
    EXPECT_TRUE(request.value()->accept_gzip);
  }
}
TEST(Unary, ErrorDetailsRoundTrip) {
  Status status{Code::kPermissionDenied,
                "quotes \" and unicode 🌍",
                {{"example.Detail", std::string("\0\xff", 2)}}};
  auto json = EncodeError(status);
  ASSERT_TRUE(json.ok());
  auto decoded = DecodeError(*json.value());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value()->status.code, status.code);
  EXPECT_EQ(decoded.value()->status.message, status.message);
  ASSERT_EQ(decoded.value()->status.details.size(), 1);
  EXPECT_EQ(decoded.value()->status.details.front().value,
            status.details.front().value);
  EXPECT_TRUE(DecodeError("{}").ok());
  EXPECT_TRUE(DecodeError("{\"code\":null}").ok());
  EXPECT_FALSE(DecodeError("{\"code\":\"internal\",\"details\":[{\"type\":"
                           "\"x\",\"value\":\"!\"}]}")
                   .ok());
}
TEST(Unary, HttpFallbackAndMetadataSurviveErrors) {
  auto decoded = DecodeUnaryResponse({0,
                                      {{":status", "503"},
                                       {"content-type", "text/html"},
                                       {"request-id", "123"},
                                       {"trailer-cost", "5"}},
                                      "<html>bad gateway</html>"},
                                     Codec::kProtobuf, 1024);
  EXPECT_EQ(decoded.status.code, Code::kUnavailable);
  ASSERT_EQ(decoded.headers.size(), 1);
  ASSERT_EQ(decoded.trailers.size(), 1);
  EXPECT_EQ(decoded.trailers.front().first, "cost");
  auto response = EncodeUnaryResponse(
      {{Code::kInvalidArgument, "bad request", {}}, "", {}, {{"cost", "7"}}},
      Codec::kProtobuf, true);
  ASSERT_TRUE(response.ok());
  decoded =
      DecodeUnaryResponse(std::move(*response.value()), Codec::kProtobuf, 1024);
  EXPECT_EQ(decoded.status.code, Code::kInvalidArgument);
  EXPECT_EQ(decoded.status.message, "bad request");
  EXPECT_EQ(decoded.trailers.front().second, "7");
}
TEST(Unary, UnknownErrorCodesUseHttpFallbackAndPreserveMessage) {
  for (std::string json :
       {R"({"message":"oops"})", R"({"code":null,"message":"oops"})",
        R"({"code":"future_code","message":"oops"})"}) {
    auto response = DecodeUnaryResponse(
        {0, {{":status", "503"}, {"content-type", "application/json"}}, json},
        Codec::kProtobuf, 1024);
    EXPECT_EQ(response.status.code, Code::kUnavailable);
    EXPECT_EQ(response.status.message, "oops");
  }
  auto known = DecodeError(R"({"code":"unknown"})", Code::kUnavailable);
  ASSERT_TRUE(known.ok());
  EXPECT_EQ(known.value()->status.code, Code::kUnknown);
}
TEST(Unary, RequestedGzipIncludesEmptyMessages) {
  CallOptions options;
  options.compress = true;
  auto encoded = EncodeUnaryRequest("/test/Echo", "", options);
  ASSERT_TRUE(encoded.ok());
  EXPECT_FALSE(encoded.value()->body.empty());
  auto decoded = DecodeUnaryRequest(std::move(*encoded.value()), 1024);
  ASSERT_TRUE(decoded.ok());
  EXPECT_TRUE(decoded.value()->payload.empty());
}
TEST(Unary, CompressionAndMetadataRoundTrip) {
  CallOptions options;
  options.compress = true;
  options.headers = {
      {"x-value", "one"}, {"x-value", "two"}, {"trace-bin", "YQ"}};
  options.transport.deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  auto request = EncodeUnaryRequest("/test.Service/Echo",
                                    std::string(10000, 'x'), options);
  ASSERT_TRUE(request.ok());
  auto decoded = DecodeUnaryRequest(std::move(*request.value()), 10000);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message;
  EXPECT_EQ(decoded.value()->payload, std::string(10000, 'x'));
  EXPECT_TRUE(decoded.value()->accept_gzip);
  EXPECT_EQ(decoded.value()->headers, options.headers);
}
TEST(Unary, ConcatenatedGzipRequestAndResponse) {
  for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
    Text input;
    input.set_value("multiple gzip members");
    auto payload = EncodeMessage(input, codec);
    ASSERT_TRUE(payload.ok());
    const size_t middle = payload.value()->size() / 2;
    auto first = Gzip(payload.value()->substr(0, middle));
    auto second = Gzip(payload.value()->substr(middle));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    CallOptions options;
    options.codec = codec;
    options.compress = true;
    auto request = EncodeUnaryRequest("/test.Service/Echo", "", options);
    ASSERT_TRUE(request.ok());
    request.value()->body = *first.value() + *second.value();
    auto decoded = DecodeUnaryRequest(std::move(*request.value()), 1024);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message;
    EXPECT_EQ(decoded.value()->payload, *payload.value());
    auto response =
        EncodeUnaryResponse({{}, *payload.value(), {}, {}}, codec, true);
    ASSERT_TRUE(response.ok());
    response.value()->body = *first.value() + *second.value();
    auto returned =
        DecodeUnaryResponse(std::move(*response.value()), codec, 1024);
    ASSERT_TRUE(returned.status.ok()) << returned.status.message;
    Text output;
    ASSERT_TRUE(DecodeMessage(returned.payload, codec, output).ok());
    EXPECT_EQ(output.value(), input.value());
  }
}
TEST(Unary, ReservedHeadersAndBadBinaryMetadataRejected) {
  for (Metadata headers :
       {Metadata{{"content-type", "text/plain"}},
        Metadata{{"connect-timeout-ms", "123"}}, Metadata{{"value-bin", "!"}},
        Metadata{{"Uppercase", "x"}}}) {
    CallOptions options;
    options.headers = std::move(headers);
    EXPECT_FALSE(EncodeUnaryRequest("/test/Echo", "", options).ok());
  }
}
TEST(Unary, TimeoutsAndDuplicateSingletons) {
  for (std::string value : {"", "0", "-1", "+1", "1.0", "10000000000", "1x"}) {
    auto request = EncodeUnaryRequest("/test/Echo", "", {});
    ASSERT_TRUE(request.ok());
    request.value()->headers.emplace_back("connect-timeout-ms", value);
    auto decoded = DecodeUnaryRequest(std::move(*request.value()), 1024);
    EXPECT_FALSE(decoded.ok());
    if (value == "0") EXPECT_EQ(decoded.status().code, Code::kDeadlineExceeded);
  }
  auto request = EncodeUnaryRequest("/test/Echo", "", {});
  request.value()->headers.emplace_back("content-type", "application/json");
  EXPECT_FALSE(DecodeUnaryRequest(std::move(*request.value()), 1024).ok());
}
TEST(Unary, JsonContentTypeParameters) {
  EXPECT_TRUE(
      ParseUnaryContentType("application/json; ignored=\"x; charset=latin-1\"")
          .ok());
  EXPECT_TRUE(
      ParseUnaryContentType("Application/JSON; charset=\"UTF-8\"; other=value")
          .ok());
  EXPECT_FALSE(ParseUnaryContentType("application/json; charset=latin-1").ok());
  EXPECT_TRUE(ParseUnaryContentType("application/proto; charset=latin-1").ok());
}
TEST(Unary, EmptyGzipBodyIsNotDecompressed) {
  auto request = EncodeUnaryRequest("/test/Echo", "", {});
  request.value()->headers.emplace_back("content-encoding", "gzip");
  EXPECT_TRUE(DecodeUnaryRequest(std::move(*request.value()), 1024).ok());
}
TEST(Rpc, TypedRoundTripAndErrorsForBothCodecs) {
  for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
    asio::io_context loop;
    Server server;
    ASSERT_TRUE(
        (server.RegisterUnary<Text, Text>(
             "/test.Service/Echo",
             [](Request<Text> request) -> asio::awaitable<Response<Text>> {
               Response<Text> response;
               response.headers = request.headers;
               response.trailers = {{"cost", "1"}};
               if (request.message.value() == "error") {
                 response.status = {Code::kPermissionDenied,
                                    "denied",
                                    {{"example.Empty", ""}}};
               } else
                 response.message = std::move(request.message);
               co_return response;
             }))
            .ok());
    Client client([&server](HttpMessage request, TransportOptions options)
                      -> asio::awaitable<Result<HttpMessage>> {
      co_return Result<HttpMessage>(
          co_await server.Handle(std::move(request), options.max_body_bytes));
    });
    int completed = 0;
    for (std::string value : {"hello", "error"}) {
      Text input;
      input.set_value(value);
      CallOptions options;
      options.codec = codec;
      options.compress = true;
      options.headers = {{"request-id", "123"}};
      asio::co_spawn(
          loop, client.Unary<Text, Text>("/test.Service/Echo", input, options),
          [&completed, value](auto exception, Response<Text> response) {
            EXPECT_FALSE(exception);
            EXPECT_EQ(response.headers, (Metadata{{"request-id", "123"}}));
            EXPECT_EQ(response.trailers, (Metadata{{"cost", "1"}}));
            if (value == "error") {
              EXPECT_EQ(response.status.code, Code::kPermissionDenied);
              EXPECT_FALSE(response.message);
              EXPECT_EQ(response.status.details.size(), 1);
            } else {
              ASSERT_TRUE(response.status.ok()) << response.status.message;
              ASSERT_TRUE(response.message);
              EXPECT_EQ(response.message->value(), value);
            }
            ++completed;
          });
    }
    loop.run();
    EXPECT_EQ(completed, 2);
  }
}
TEST(Rpc, RejectsUnsupportedMediaTypeWithoutCallingHandler) {
  asio::io_context loop;
  Server server;
  auto request = EncodeUnaryRequest("/test/Echo", "", {});
  for (auto& [name, value] : request.value()->headers) {
    if (name == "content-type") value = "application/json; charset=latin-1";
  }
  bool done = false;
  asio::co_spawn(loop, server.Handle(std::move(*request.value())),
                 [&done](auto exception, HttpMessage response) {
                   EXPECT_FALSE(exception);
                   EXPECT_EQ(response.headers.front().second, "415");
                   done = true;
                 });
  loop.run();
  EXPECT_TRUE(done);
}
TEST(Rpc, DeadlineCancelsAndJoinsHandler) {
  asio::io_context loop;
  Server server;
  bool canceled = false;
  ASSERT_TRUE(
      (server.RegisterUnary<Text, Text>(
           "/test/Slow",
           [&canceled](Request<Text>) -> asio::awaitable<Response<Text>> {
             co_await asio::this_coro::throw_if_cancelled(false);
             asio::steady_timer timer(co_await asio::this_coro::executor);
             timer.expires_after(std::chrono::seconds(10));
             auto [error] =
                 co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
             canceled = error == asio::error::operation_aborted;
             Response<Text> response;
             response.message.emplace();
             co_return response;
           }))
          .ok());
  CallOptions options;
  options.transport.deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
  auto request = EncodeUnaryRequest("/test/Slow", "", options);
  ASSERT_TRUE(request.ok());
  bool done = false;
  asio::co_spawn(loop, server.Handle(std::move(*request.value())),
                 [&](auto exception, HttpMessage response) {
                   EXPECT_FALSE(exception);
                   EXPECT_TRUE(canceled);
                   EXPECT_EQ(DecodeUnaryResponse(std::move(response),
                                                 Codec::kProtobuf, 1024)
                                 .status.code,
                             Code::kDeadlineExceeded);
                   done = true;
                 });
  loop.run();
  EXPECT_TRUE(done);
}
TEST(Rpc, TypedHttp2Loopback) {
  asio::io_context loop;
  Server server;
  ASSERT_TRUE(
      (server.RegisterUnary<Text, Text>(
           "/test.Service/Echo",
           [](Request<Text> request) -> asio::awaitable<Response<Text>> {
             Response<Text> response;
             response.message = std::move(request.message);
             response.trailers = {{"cost", "1"}};
             co_return response;
           }))
          .ok());
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
  asio::ip::tcp::socket socket(loop);
  socket.connect(endpoint, error);
  ASSERT_FALSE(error);
  asio::ip::tcp::socket accepted(loop);
  acceptor.accept(accepted, error);
  ASSERT_FALSE(error);
  Client client([&socket](HttpMessage request, TransportOptions options)
                    -> asio::awaitable<Result<HttpMessage>> {
    request.headers.insert(request.headers.begin(),
                           {{":scheme", "http"}, {":authority", "localhost"}});
    co_return co_await UnaryHttp2(std::move(socket), std::move(request.headers),
                                  std::move(request.body), options);
  });
  asio::co_spawn(
      loop,
      ServeHttp2(
          std::move(accepted),
          [&server](HttpMessage request) -> asio::awaitable<HttpMessage> {
            co_return co_await server.Handle(std::move(request));
          }),
      [](auto exception, Status status) {
        EXPECT_FALSE(exception);
        EXPECT_TRUE(status.ok()) << status.message;
      });
  Text input;
  input.set_value("typed network RPC");
  CallOptions options;
  options.compress = true;
  options.transport.deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  bool done = false;
  asio::co_spawn(loop,
                 client.Unary<Text, Text>("/test.Service/Echo", input, options),
                 [&done](auto exception, Response<Text> response) {
                   EXPECT_FALSE(exception);
                   ASSERT_TRUE(response.status.ok()) << response.status.message;
                   ASSERT_TRUE(response.message);
                   EXPECT_EQ(response.message->value(), "typed network RPC");
                   EXPECT_EQ(response.trailers, (Metadata{{"cost", "1"}}));
                   done = true;
                 });
  loop.run();
  EXPECT_TRUE(done);
}
}  // namespace
}  // namespace connectrpc
