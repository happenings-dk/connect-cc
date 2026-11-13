#include "connectrpc/grpc_web_protocol.h"

#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/escaping.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
TEST(GrpcWebProtocol, NegotiatesIndependentRequestAndResponseEncoding) {
  for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
    for (bool text : {false, true}) {
      for (bool accept_text : {false, true}) {
        StreamingRequest request;
        request.procedure = "/example.Service/Call";
        request.protocol = Protocol::kGrpcWeb;
        request.codec = codec;
        request.text = text;
        request.accept_text = accept_text;
        request.gzip = true;
        request.headers = {{"custom", "yes"}};
        auto wire = EncodeStreamingRequest(request);
        ASSERT_TRUE(wire.ok());
        auto decoded = DecodeStreamingRequest(*wire.value());
        ASSERT_TRUE(decoded.ok()) << decoded.status().message;
        EXPECT_EQ(decoded.value()->protocol, Protocol::kGrpcWeb);
        EXPECT_EQ(decoded.value()->codec, codec);
        EXPECT_EQ(decoded.value()->text, text);
        EXPECT_EQ(decoded.value()->accept_text, accept_text);
        EXPECT_EQ(decoded.value()->headers, request.headers);
        auto response = EncodeStreamingResponse(
            {codec, request.headers, true, Protocol::kGrpcWeb, accept_text});
        ASSERT_TRUE(response.ok());
        auto parsed = DecodeGrpcWebResponse(*response.value(), codec, true);
        ASSERT_TRUE(parsed.ok());
        EXPECT_EQ(parsed.value()->text, accept_text);
        EXPECT_EQ(parsed.value()->headers, request.headers);
        EXPECT_FALSE(
            DecodeGrpcWebResponse(*response.value(), codec, false).ok());
      }
    }
  }
  EXPECT_TRUE(ParseGrpcWebContentType("application/grpc-web").ok());
  EXPECT_TRUE(ParseGrpcWebContentType("application/grpc-web-text").ok());
  EXPECT_FALSE(ParseGrpcWebContentType("application/grpc-web-textual").ok());
  EXPECT_FALSE(ParseGrpcWebContentType("application/grpc+proto").ok());
}
TEST(GrpcWebProtocol, Base64SegmentsMaySplitInsideFrames) {
  std::string bytes("\0\xff\x80hello", 8);
  for (size_t segment = 1; segment <= bytes.size(); ++segment) {
    std::string wire;
    for (size_t i = 0; i < bytes.size(); i += segment)
      wire += absl::Base64Escape(std::string_view(bytes).substr(i, segment));
    for (size_t chunk = 1; chunk <= wire.size(); ++chunk) {
      GrpcWebTextDecoder decoder;
      std::string decoded;
      for (size_t i = 0; i < wire.size(); i += chunk) {
        auto part = decoder.Read(std::string_view(wire).substr(i, chunk));
        ASSERT_TRUE(part.ok());
        decoded += *part.value();
      }
      EXPECT_TRUE(decoder.Finish().ok());
      EXPECT_EQ(decoded, bytes);
    }
  }
  for (std::string_view invalid :
       {"=AAA", "A=AA", "AA=A", "%%%%", "A", "AA", "AAA", "AA==!"}) {
    GrpcWebTextDecoder decoder;
    auto decoded = decoder.Read(invalid);
    EXPECT_TRUE(!decoded.ok() || !decoder.Finish().ok()) << invalid;
    if (!decoded.ok()) EXPECT_FALSE(decoder.Read("AAAA").ok());
  }
}
TEST(GrpcWebProtocol, FragmentedTerminalStatusAndCompressedTrailers) {
  StreamCompletion end{{Code::kPermissionDenied,
                        "no % access\n",
                        {{"example.Detail", "detail"}}},
                       {{"cost", "1"}, {"cost", "2"}}};
  auto terminal = EncodeGrpcWebTrailers(end);
  ASSERT_TRUE(terminal.ok());
  auto message = EncodeStreamingMessage("hello", true);
  ASSERT_TRUE(message.ok());
  for (bool compress_terminal : {false, true}) {
    std::string terminal_wire = *terminal.value();
    if (compress_terminal) {
      auto zipped = Gzip(std::string_view(terminal_wire).substr(5));
      ASSERT_TRUE(zipped.ok());
      terminal_wire =
          *EncodeEnvelope({true, true, *zipped.value()}, Protocol::kGrpcWeb)
               .value();
    }
    std::string wire = *message.value() + terminal_wire;
    for (size_t chunk = 1; chunk <= wire.size(); ++chunk) {
      StreamingDecoder decoder(true, true, {}, Protocol::kGrpcWeb);
      std::vector<std::string> messages;
      for (size_t i = 0; i < wire.size(); i += chunk) {
        std::string_view part = std::string_view(wire).substr(i, chunk);
        while (!part.empty()) {
          auto next = decoder.ReadOne(part);
          ASSERT_TRUE(next.ok()) << next.status().message;
          if (*next.value()) messages.push_back(**next.value());
        }
      }
      EXPECT_EQ(messages, (std::vector<std::string>{"hello"}));
      auto finished = decoder.Finish();
      ASSERT_TRUE(finished.ok());
      EXPECT_EQ(finished.value()->status.code, end.status.code);
      EXPECT_EQ(finished.value()->status.message, end.status.message);
      EXPECT_EQ(finished.value()->status.details.size(), 1);
      EXPECT_EQ(finished.value()->trailers, end.trailers);
    }
  }
}
TEST(GrpcWebProtocol, RejectsMalformedMissingAndOversizedTrailers) {
  for (std::string_view bad :
       {"", "grpc-status: 0\r\ngrpc-status: 0", "grpc-status: 17",
        "grpc-status: 0\r\n\r\n", "grpc-status: 0\ncustom: x",
        "Bad[: x\r\ngrpc-status: 0", ":status: 200\r\ngrpc-status: 0"})
    EXPECT_FALSE(DecodeGrpcWebTrailers(bad).ok()) << bad;
  EXPECT_TRUE(DecodeGrpcWebTrailers("grpc-status: 0").ok());
  auto mixed = DecodeGrpcWebTrailers(
      "Grpc-Status: 9\r\ngRPC-Message: error\r\nx-Custom: yes\r\n");
  ASSERT_TRUE(mixed.ok());
  EXPECT_EQ(mixed.value()->status.code, Code::kFailedPrecondition);
  EXPECT_EQ(mixed.value()->trailers, (Metadata{{"x-custom", "yes"}}));
  StreamingDecoder missing(true, false, {}, Protocol::kGrpcWeb);
  EXPECT_FALSE(missing.Finish().ok());
  auto terminal = EncodeGrpcWebTrailers({});
  ASSERT_TRUE(terminal.ok());
  StreamingDecoder request(false, false, {}, Protocol::kGrpcWeb);
  std::string_view bytes = *terminal.value();
  EXPECT_FALSE(request.ReadOne(bytes).ok());
  StreamingDecoder after(true, false, {}, Protocol::kGrpcWeb);
  std::string joined =
      *terminal.value() + *EncodeStreamingMessage("late", false).value();
  bytes = joined;
  ASSERT_TRUE(after.ReadOne(bytes).ok());
  EXPECT_FALSE(after.ReadOne(bytes).ok());
  StreamLimits limits;
  limits.max_metadata_bytes = 10;
  EXPECT_FALSE(EncodeGrpcWebTrailers({}, limits).ok());
  EXPECT_FALSE(DecodeGrpcWebTrailers("grpc-status: 0", limits).ok());
}
}  // namespace
}  // namespace connectrpc
