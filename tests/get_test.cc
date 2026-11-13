#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <string>
#include <utility>

#include "connectrpc/rpc.h"
#include "connectrpc/unary.h"
#include "google/protobuf/struct.pb.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
HttpMessage Get(std::string query) {
  return {
      0, {{":method", "GET"}, {":path", "/test.Service/Read?" + query}}, {}};
}
std::string Field(const Metadata& headers, std::string_view name) {
  for (const auto& [key, value] : headers)
    if (key == name) return value;
  return {};
}
TEST(Get, EncodesCanonicalTargetAndEmptyBody) {
  CallOptions options;
  options.use_get = true;
  options.codec = Codec::kJson;
  options.headers = {{"custom", "value"}};
  auto request = EncodeUnaryRequest("/test.Service/Read", "\"a +&=/#å\"",
                                    options, {Idempotency::kNoSideEffects});
  ASSERT_TRUE(request.ok()) << request.status().message;
  EXPECT_EQ(Field(request.value()->headers, ":method"), "GET");
  EXPECT_EQ(
      Field(request.value()->headers, ":path"),
      "/test.Service/"
      "Read?connect=v1&encoding=json&message=%22a%20%2B%26%3D%2F%23%C3%A5%22");
  EXPECT_TRUE(request.value()->body.empty());
  EXPECT_TRUE(Field(request.value()->headers, "content-type").empty());
  EXPECT_TRUE(Field(request.value()->headers, "content-encoding").empty());
  EXPECT_TRUE(
      Field(request.value()->headers, "connect-protocol-version").empty());
  auto decoded = DecodeUnaryRequest(*request.value(), 1024);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message;
  EXPECT_TRUE(decoded.value()->is_get);
  EXPECT_EQ(decoded.value()->procedure, "/test.Service/Read");
  EXPECT_EQ(decoded.value()->payload, "\"a +&=/#å\"");
  EXPECT_EQ(decoded.value()->headers, options.headers);
}
TEST(Get, ProtobufGzipRoundTripAndLimits) {
  std::string bytes("\0\xff+\x80", 4);
  for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
    for (bool gzip : {false, true}) {
      std::string payload = codec == Codec::kProtobuf ? bytes : "\"a value\"";
      CallOptions options;
      options.use_get = true;
      options.codec = codec;
      options.compress = gzip;
      auto request = EncodeUnaryRequest("/test.Service/Read", payload, options,
                                        {Idempotency::kNoSideEffects});
      ASSERT_TRUE(request.ok());
      std::string target = Field(request.value()->headers, ":path");
      EXPECT_EQ(target.find("base64=1") != std::string::npos,
                gzip || codec == Codec::kProtobuf);
      auto decoded = DecodeUnaryRequest(*request.value(), 1024);
      ASSERT_TRUE(decoded.ok()) << decoded.status().message;
      EXPECT_EQ(decoded.value()->payload, payload);
      EXPECT_EQ(DecodeUnaryRequest(*request.value(), 2).status().code,
                Code::kResourceExhausted);
    }
  }
  CallOptions options;
  options.use_get = true;
  EXPECT_EQ(EncodeUnaryRequest("/s/m", "", options).status().code,
            Code::kInvalidArgument);
  EXPECT_EQ(EncodeUnaryRequest("/s/m", "", options, {Idempotency::kIdempotent})
                .status()
                .code,
            Code::kInvalidArgument);
  options.transport.max_metadata_bytes = 30;
  EXPECT_EQ(
      EncodeUnaryRequest("/s/m", "", options, {Idempotency::kNoSideEffects})
          .status()
          .code,
      Code::kResourceExhausted);
}
TEST(Get, QueryOrderUnknownFieldsPaddingAndPlus) {
  auto long_key = DecodeUnaryRequest(
      Get(std::string(1024, 'x') + "=ignored&encoding=proto&message="), 0);
  EXPECT_TRUE(long_key.ok()) << long_key.status().message;
  for (const auto& query :
       {"message=%22hello+world%22&ignored=foo&encoding=json&connect=v1",
        "base64=ignored&encoding=json&message=%22hello%20world%22",
        "encoding=json&message=ImhlbGxvIHdvcmxkIg%3D%3D&base64=1",
        "base64=1&message=ImhlbGxvIHdvcmxkIg&encoding=json"}) {
    auto decoded = DecodeUnaryRequest(Get(query), 1024);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message << " for " << query;
    EXPECT_EQ(decoded.value()->payload, "\"hello world\"");
  }
  auto empty = DecodeUnaryRequest(Get("encoding=proto&message="), 0);
  ASSERT_TRUE(empty.ok());
  EXPECT_TRUE(empty.value()->payload.empty());
  auto compressed_empty = DecodeUnaryRequest(
      Get("encoding=proto&compression=gzip&message=&base64=1"), 0);
  ASSERT_TRUE(compressed_empty.ok());
  EXPECT_TRUE(compressed_empty.value()->payload.empty());
  EXPECT_TRUE(compressed_empty.value()->accept_gzip);
}
TEST(Get, RejectsMalformedQueriesAndBodies) {
  for (const auto& query :
       {"message=x", "encoding=json", "encoding=json&message=%xx",
        "encoding=json&message=%", "encoding=json&message=%00%ff",
        "encoding=json&message=%c0%80", "encoding=json&message=%ed%a0%80",
        "encoding=json&message=%f4%90%80%80", "encoding=json&message=%e2%82",
        "encoding=json&encoding=proto&message=x",
        "encoding=json&message=x&message=y",
        "encoding=json&message=x&connect=v2",
        "encoding=json&message=!!!&base64=1",
        "encoding=json&message=YQ===&base64=1",
        "encoding=json&message=Y&base64=1",
        "encoding=json&message=YQ%20%3d%3d&base64=1"}) {
    EXPECT_EQ(DecodeUnaryRequest(Get(query), 1024).status().code,
              Code::kInvalidArgument)
        << query;
  }
  auto request = Get("encoding=proto&message=");
  request.body = "body";
  EXPECT_EQ(DecodeUnaryRequest(std::move(request), 1024).status().code,
            Code::kInvalidArgument);
  EXPECT_EQ(
      DecodeUnaryRequest(Get("encoding=json&message=x&compression=br"), 1024)
          .status()
          .code,
      Code::kUnimplemented);
  EXPECT_EQ(
      DecodeUnaryRequest(Get("encoding=xml&message=x"), 1024).status().code,
      Code::kUnimplemented);
}
TEST(Get, DeterministicMapSerialization) {
  google::protobuf::Struct a;
  google::protobuf::Struct b;
  (*a.mutable_fields())["z"].set_number_value(1.25);
  (*a.mutable_fields())["a"].set_string_value("quotes \" and \n");
  (*b.mutable_fields())["a"] = a.fields().at("a");
  (*b.mutable_fields())["z"] = a.fields().at("z");
  for (Codec codec : {Codec::kJson, Codec::kProtobuf}) {
    auto expected = EncodeDeterministicMessage(a, codec);
    ASSERT_TRUE(expected.ok());
    for (int i = 0; i < 20; ++i) {
      auto actual = EncodeDeterministicMessage(b, codec);
      ASSERT_TRUE(actual.ok());
      EXPECT_EQ(*actual.value(), *expected.value());
    }
    google::protobuf::Struct decoded;
    EXPECT_TRUE(DecodeMessage(*expected.value(), codec, decoded).ok());
    EXPECT_EQ(decoded.fields().at("a").string_value(),
              a.fields().at("a").string_value());
    EXPECT_EQ(decoded.fields().at("z").number_value(), 1.25);
  }
}
TEST(Get, RouterEnforcesEligibilityAndHttpErrors) {
  using Message = google::protobuf::StringValue;
  asio::io_context loop;
  Server server;
  int calls = 0;
  auto handler =
      [&](Request<Message> request) -> asio::awaitable<Response<Message>> {
    ++calls;
    co_return Response<Message>{{}, std::move(request.message), {}, {}};
  };
  ASSERT_TRUE(
      (server.RegisterUnary<Message, Message>("/test.Service/Read", handler,
                                              {Idempotency::kNoSideEffects}))
          .ok());
  ASSERT_TRUE((server.RegisterUnary<Message, Message>(
                   "/test.Service/Write", handler, {Idempotency::kIdempotent}))
                  .ok());
  bool done = false;
  auto run = [&]() -> asio::awaitable<void> {
    auto response =
        co_await server.Handle(Get("encoding=json&message=%22yes%22"));
    EXPECT_EQ(Field(response.headers, ":status"), "200");
    EXPECT_EQ(response.body, "\"yes\"");
    auto bad = co_await server.Handle(Get("encoding=xml&message=x"));
    EXPECT_EQ(Field(bad.headers, ":status"), "415");
    auto write = Get("encoding=json&message=%22no%22");
    write.headers[1].second.replace(0, std::string("/test.Service/Read").size(),
                                    "/test.Service/Write");
    bad = co_await server.Handle(std::move(write));
    EXPECT_EQ(Field(bad.headers, ":status"), "405");
    EXPECT_EQ(Field(bad.headers, "allow"), "POST");
    auto timeout = Get("encoding=json&message=%22no%22");
    timeout.headers.emplace_back("connect-timeout-ms", "0");
    bad = co_await server.Handle(std::move(timeout));
    EXPECT_EQ(Field(bad.headers, ":status"), "504");
    done = true;
  };
  asio::co_spawn(loop, run(), [](auto exception) { EXPECT_FALSE(exception); });
  loop.run();
  EXPECT_TRUE(done);
  EXPECT_EQ(calls, 1);
}
}  // namespace
}  // namespace connectrpc
