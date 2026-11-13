#include "connectrpc/streaming_protocol.h"

#include <string>
#include <string_view>
#include <vector>

#include "connectrpc/codec.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
TEST(StreamingProtocol, HeaderNegotiationAndTimeout) {
  for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
    StreamingRequest request;
    request.procedure = "/example.Service/Stream";
    request.codec = codec;
    request.gzip = true;
    request.headers = {{"custom", "value"}};
    request.deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto headers = EncodeStreamingRequest(request);
    ASSERT_TRUE(headers.ok());
    auto decoded = DecodeStreamingRequest(*headers.value());
    ASSERT_TRUE(decoded.ok()) << decoded.status().message;
    EXPECT_EQ(decoded.value()->procedure, request.procedure);
    EXPECT_EQ(decoded.value()->codec, codec);
    EXPECT_TRUE(decoded.value()->gzip);
    EXPECT_TRUE(decoded.value()->accept_gzip);
    EXPECT_EQ(decoded.value()->headers, request.headers);
    EXPECT_GT(decoded.value()->deadline, std::chrono::steady_clock::now());
    auto response = EncodeStreamingResponse({codec, request.headers, true});
    ASSERT_TRUE(response.ok());
    auto result = DecodeStreamingResponse(*response.value(), codec, true);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value()->headers, request.headers);
    EXPECT_FALSE(DecodeStreamingResponse(*response.value(), codec, false).ok());
    response.value()->emplace_back("content-type", "application/connect+json");
    EXPECT_FALSE(DecodeStreamingResponse(*response.value(), codec, true).ok());
  }
  StreamingRequest reserved;
  reserved.procedure = "/example.Service/Stream";
  reserved.headers = {{"connect-content-encoding", "br"}};
  EXPECT_FALSE(EncodeStreamingRequest(reserved).ok());
}
TEST(StreamingProtocol, TerminalStatusDetailsAndMetadata) {
  StreamCompletion completion;
  completion.status = {
      Code::kPermissionDenied, "no access", {{"example.Detail", "\1\2"}}};
  completion.trailers = {{"cost", "1"}, {"cost", "2"}, {"data-bin", "AQI"}};
  auto wire = EncodeEndStream(completion);
  ASSERT_TRUE(wire.ok());
  StreamingDecoder decoder(true, false);
  std::string_view bytes = *wire.value();
  auto value = decoder.ReadOne(bytes);
  ASSERT_TRUE(value.ok());
  EXPECT_FALSE(*value.value());
  auto result = decoder.Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value()->status.code, Code::kPermissionDenied);
  EXPECT_EQ(result.value()->status.message, "no access");
  ASSERT_EQ(result.value()->status.details.size(), 1);
  EXPECT_EQ(result.value()->status.details.front().value, "\1\2");
  EXPECT_EQ(result.value()->trailers.size(), 3);
  EXPECT_EQ(DecodeEndStream("{}").value()->status.code, Code::kOk);
  auto upper = DecodeEndStream(R"({"metadata":{"Foo":["bar"]}})");
  ASSERT_TRUE(upper.ok());
  EXPECT_EQ(upper.value()->trailers.front().first, "foo");
}
TEST(StreamingProtocol, CompressedTerminalAndTolerantErrorFields) {
  EXPECT_TRUE(DecodeEndStream(R"({"error":null})").value()->status.ok());
  for (std::string_view json : {R"({"error":{}})", R"({"error":{"code":null}})",
                                R"({"error":{"code":"future_code"}})"}) {
    auto decoded = DecodeEndStream(json);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value()->status.code, Code::kUnknown);
  }
  auto compressed =
      Gzip(R"({"error":{"code":"out_of_range","message":"oops"}})");
  ASSERT_TRUE(compressed.ok());
  auto wire =
      EncodeEnvelope({true, true, *compressed.value()}, Protocol::kConnect);
  ASSERT_TRUE(wire.ok());
  EXPECT_EQ(static_cast<unsigned char>(wire.value()->front()), 3);
  StreamingDecoder decoder(true, true);
  std::string_view bytes = *wire.value();
  ASSERT_TRUE(decoder.ReadOne(bytes).ok());
  auto complete = decoder.Finish();
  ASSERT_TRUE(complete.ok());
  EXPECT_EQ(complete.value()->status.code, Code::kOutOfRange);
  EXPECT_EQ(complete.value()->status.message, "oops");
  StreamingDecoder unnegotiated(true, false);
  bytes = *wire.value();
  EXPECT_FALSE(unnegotiated.ReadOne(bytes).ok());
  StreamLimits limits;
  limits.max_metadata_bytes = 100;
  compressed = Gzip(std::string(1000, 'x'));
  wire = EncodeEnvelope({true, true, *compressed.value()}, Protocol::kConnect);
  StreamingDecoder bomb(true, true, limits);
  bytes = *wire.value();
  EXPECT_EQ(bomb.ReadOne(bytes).status().code, Code::kResourceExhausted);
}
TEST(StreamingProtocol, RejectsMalformedTerminalAndMetadata) {
  for (std::string_view json :
       {"null", "[]", "garbage", R"({"error":{"code":5}})",
        R"({"metadata":[]})", R"({"metadata":{"bad key":[]}})",
        R"({"metadata":{"foo":"bar"}})", R"({"metadata":{"foo":[5]}})",
        R"({"metadata":{"foo":["\r\n"]}})",
        R"({"metadata":{"bad-bin":["%"]}})"}) {
    EXPECT_FALSE(DecodeEndStream(json).ok()) << json;
  }
  StreamLimits limits;
  limits.max_metadata_bytes = 2;
  EXPECT_TRUE(DecodeEndStream("{}", limits).ok());
  EXPECT_EQ(DecodeEndStream(R"({"metadata":{}})", limits).status().code,
            Code::kResourceExhausted);
}
TEST(StreamingProtocol, PullParserLeavesCoalescedMessagesUnread) {
  auto first = EncodeStreamingMessage("", false);
  auto second = EncodeStreamingMessage("payload", true);
  auto terminal = EncodeEndStream({});
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(terminal.ok());
  std::string wire = *first.value() + *second.value() + *terminal.value();
  for (size_t split = 0; split <= wire.size(); ++split) {
    StreamingDecoder decoder(true, true);
    std::vector<std::string> messages;
    for (std::string_view part : {std::string_view(wire).substr(0, split),
                                  std::string_view(wire).substr(split)}) {
      while (!part.empty()) {
        size_t before = part.size();
        auto next = decoder.ReadOne(part);
        ASSERT_TRUE(next.ok());
        EXPECT_LT(part.size(), before);
        if (*next.value()) messages.push_back(std::move(**next.value()));
      }
    }
    EXPECT_EQ(messages, (std::vector<std::string>{"", "payload"}));
    ASSERT_TRUE(decoder.Finish().ok());
    EXPECT_TRUE(decoder.Finish().value()->status.ok());
  }
  StreamingDecoder decoder(true, true);
  std::string_view all = wire;
  ASSERT_TRUE(decoder.ReadOne(all).ok());
  EXPECT_EQ(all.size(), wire.size() - first.value()->size());
  EXPECT_EQ(decoder.Finish().status().code, Code::kInternal);
}
TEST(StreamingProtocol, MissingTruncatedAndMisplacedTerminals) {
  auto terminal = EncodeEndStream({});
  ASSERT_TRUE(terminal.ok());
  StreamingDecoder request(false, false);
  std::string_view bytes = *terminal.value();
  EXPECT_EQ(request.ReadOne(bytes).status().code, Code::kInvalidArgument);
  EXPECT_FALSE(request.Finish().ok());
  StreamingDecoder response(true, false);
  EXPECT_EQ(response.Finish().status().code, Code::kInternal);
  bytes = std::string_view(*terminal.value()).substr(0, 4);
  EXPECT_TRUE(response.ReadOne(bytes).ok());
  EXPECT_EQ(response.Finish().status().code, Code::kDataLoss);
  StreamingDecoder extra(true, false);
  std::string trailing = *terminal.value() + "x";
  bytes = trailing;
  EXPECT_TRUE(extra.ReadOne(bytes).ok());
  EXPECT_FALSE(extra.ReadOne(bytes).ok());
  EXPECT_FALSE(extra.Finish().ok());
}
TEST(StreamingProtocol, IndependentMessageDecompressionAndTerminalLimits) {
  StreamLimits limits;
  limits.max_message_bytes = 100;
  limits.max_decompressed_bytes = 1000;
  limits.max_metadata_bytes = 2;
  auto wire = EncodeStreamingMessage(std::string(1000, 'x'), true, limits);
  ASSERT_TRUE(wire.ok());
  StreamingDecoder decoder(false, true, limits);
  std::string_view bytes = *wire.value();
  auto message = decoder.ReadOne(bytes);
  ASSERT_TRUE(message.ok());
  EXPECT_EQ((**message.value()).size(), 1000);
  EXPECT_TRUE(decoder.Finish().ok());
  --limits.max_decompressed_bytes;
  StreamingDecoder bomb(false, true, limits);
  bytes = *wire.value();
  EXPECT_EQ(bomb.ReadOne(bytes).status().code, Code::kResourceExhausted);
  StreamingDecoder unnegotiated(false, false);
  bytes = *wire.value();
  EXPECT_EQ(unnegotiated.ReadOne(bytes).status().code, Code::kInternal);
  StreamingDecoder oversized(true, false, limits);
  std::string header("\0\0\0\0\x65", 5);
  bytes = header;
  EXPECT_EQ(oversized.ReadOne(bytes).status().code, Code::kResourceExhausted);
  StreamingDecoder big_terminal(true, false, limits);
  header = std::string("\2\0\0\0\3", 5);
  bytes = header;
  EXPECT_EQ(big_terminal.ReadOne(bytes).status().code,
            Code::kResourceExhausted);
  EXPECT_EQ(EncodeStreamingMessage(std::string(1000, 'x'), false, limits)
                .status()
                .code,
            Code::kResourceExhausted);
}
}  // namespace
}  // namespace connectrpc
