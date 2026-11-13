#include "connectrpc/grpc_protocol.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "connectrpc/envelope.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
TEST(GrpcProtocol, TimeoutUnitsBoundsAndInvalidInput) {
  using Clock = std::chrono::steady_clock;
  for (const auto& [text, duration] :
       std::vector<std::pair<std::string, std::chrono::nanoseconds>>{
           {"1n", std::chrono::nanoseconds(1)},
           {"1u", std::chrono::microseconds(1)},
           {"1m", std::chrono::milliseconds(1)},
           {"1S", std::chrono::seconds(1)},
           {"1M", std::chrono::minutes(1)},
           {"1H", std::chrono::hours(1)}}) {
    auto before = Clock::now();
    auto parsed = DecodeGrpcTimeout(text);
    ASSERT_TRUE(parsed.ok());
    EXPECT_GE(*parsed.value(), before + duration);
    EXPECT_LE(*parsed.value(), Clock::now() + duration);
  }
  EXPECT_EQ(*DecodeGrpcTimeout("").value(), Clock::time_point::max());
  EXPECT_EQ(*DecodeGrpcTimeout("99999999H").value(), Clock::time_point::max());
  auto immediate = DecodeGrpcTimeout("0m");
  ASSERT_TRUE(immediate.ok());
  EXPECT_LE(*immediate.value(), Clock::now());
  for (std::string_view invalid :
       {"1", "1s", "-1S", "+1S", "100000000n", "1.5S", "m", "1 S"})
    EXPECT_FALSE(DecodeGrpcTimeout(invalid).ok()) << invalid;
  for (auto duration : {std::chrono::seconds(1), std::chrono::seconds(100),
                        std::chrono::seconds(100000)}) {
    auto deadline = Clock::now() + duration;
    auto encoded = EncodeGrpcTimeout(deadline);
    ASSERT_TRUE(encoded.ok());
    EXPECT_LE(encoded.value()->size(), 9);
    auto decoded = DecodeGrpcTimeout(*encoded.value());
    ASSERT_TRUE(decoded.ok());
    EXPECT_GE(*decoded.value(), deadline);
    EXPECT_LT(*decoded.value(), deadline + std::chrono::seconds(1));
  }
  EXPECT_EQ(EncodeGrpcTimeout(Clock::now()).status().code,
            Code::kDeadlineExceeded);
}
TEST(GrpcProtocol, HeadersCodecsAndMetadata) {
  for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
    StreamingRequest request;
    request.protocol = Protocol::kGrpc;
    request.procedure = "/example.Service/Call";
    request.codec = codec;
    request.gzip = true;
    request.headers = {{"trace-bin", "AQI"}, {"custom", "a"}, {"custom", "b"}};
    auto encoded = EncodeStreamingRequest(request);
    ASSERT_TRUE(encoded.ok());
    auto decoded = DecodeStreamingRequest(*encoded.value());
    ASSERT_TRUE(decoded.ok()) << decoded.status().message;
    EXPECT_EQ(decoded.value()->protocol, Protocol::kGrpc);
    EXPECT_EQ(decoded.value()->codec, codec);
    EXPECT_EQ(decoded.value()->headers, request.headers);
    EXPECT_TRUE(decoded.value()->gzip);
    EXPECT_TRUE(decoded.value()->accept_gzip);
    auto response =
        EncodeGrpcResponse({codec, request.headers, true, Protocol::kGrpc});
    ASSERT_TRUE(response.ok());
    auto parsed = DecodeGrpcResponse(*response.value(), codec, true);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value()->headers, request.headers);
    EXPECT_FALSE(DecodeGrpcResponse(*response.value(), codec, false).ok());
    encoded.value()->emplace_back("grpc-encoding", "gzip");
    EXPECT_FALSE(DecodeGrpcRequest(*encoded.value()).ok());
  }
  EXPECT_EQ(*ParseGrpcContentType("application/grpc").value(),
            Codec::kProtobuf);
  EXPECT_FALSE(ParseGrpcContentType("application/grpc-web+proto").ok());
  EXPECT_FALSE(
      ParseGrpcContentType("application/grpc+json; charset=latin1").ok());
  EXPECT_TRUE(
      ParseGrpcContentType("application/grpc+json; charset=utf-8").ok());
  StreamingRequest reserved;
  reserved.procedure = "/example.Service/Call";
  reserved.headers = {{"grpc-status", "0"}};
  EXPECT_FALSE(EncodeGrpcRequest(reserved).ok());
}
TEST(GrpcProtocol, StatusDetailsPercentEncodingAndBinaryMetadata) {
  StreamCompletion completion;
  completion.status = {Code::kPermissionDenied,
                       "no % access\n\xE2\x98\x83",
                       {{"example.Detail", std::string("\0\1", 2)}}};
  completion.trailers = {{"cost", "1"}, {"cost", "2"}, {"data-bin", "AQI="}};
  auto encoded = EncodeGrpcTrailers(completion);
  ASSERT_TRUE(encoded.ok());
  EXPECT_EQ((*encoded.value())[1].second, "no %25 access%0A%E2%98%83");
  auto decoded = DecodeGrpcTrailers(*encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.status().message;
  EXPECT_EQ(decoded.value()->status.code, completion.status.code);
  EXPECT_EQ(decoded.value()->status.message, completion.status.message);
  ASSERT_EQ(decoded.value()->status.details.size(), 1);
  EXPECT_EQ(decoded.value()->status.details.front().type, "example.Detail");
  EXPECT_EQ(decoded.value()->status.details.front().value,
            std::string("\0\1", 2));
  EXPECT_EQ(decoded.value()->trailers, completion.trailers);
  auto tolerant = DecodeGrpcTrailers({{"grpc-status", "13"},
                                      {"grpc-message", "%x%20end%"},
                                      {"data-bin", "AQI=, AwQ"}});
  ASSERT_TRUE(tolerant.ok());
  EXPECT_EQ(tolerant.value()->status.message, "%x end%");
  EXPECT_EQ(tolerant.value()->trailers,
            (Metadata{{"data-bin", "AQI="}, {"data-bin", "AwQ"}}));
  (*encoded.value())[0].second = "3";
  EXPECT_EQ(DecodeGrpcTrailers(*encoded.value()).status().code,
            Code::kInternal);
  EXPECT_FALSE(DecodeGrpcTrailers(
                   {{"grpc-status", "3"}, {"grpc-status-details-bin", "%%%"}})
                   .ok());
  StreamLimits limits;
  limits.max_metadata_bytes = 10;
  EXPECT_EQ(EncodeGrpcTrailers(completion, limits).status().code,
            Code::kResourceExhausted);
  EXPECT_EQ(DecodeGrpcTrailers(*encoded.value(), limits).status().code,
            Code::kResourceExhausted);
}
TEST(GrpcProtocol, MissingMalformedAndDuplicateStatusCannotSucceed) {
  for (const auto& headers :
       std::vector<Metadata>{{},
                             {{"grpc-status", ""}},
                             {{"grpc-status", "oops"}},
                             {{"grpc-status", "17"}},
                             {{"grpc-status", "-1"}},
                             {{"grpc-status", "0"}, {"grpc-status", "0"}}})
    EXPECT_FALSE(DecodeGrpcTrailers(headers).ok());
  EXPECT_TRUE(DecodeGrpcTrailers({{"grpc-status", "0"}}).value()->status.ok());
  EXPECT_EQ(GrpcCodeFromHttpStatus(400), Code::kInternal);
  EXPECT_EQ(GrpcCodeFromHttpStatus(401), Code::kUnauthenticated);
  EXPECT_EQ(GrpcCodeFromHttpStatus(403), Code::kPermissionDenied);
  EXPECT_EQ(GrpcCodeFromHttpStatus(404), Code::kUnimplemented);
  for (int status : {429, 502, 503, 504})
    EXPECT_EQ(GrpcCodeFromHttpStatus(status), Code::kUnavailable);
  for (int status : {200, 418, 500})
    EXPECT_EQ(GrpcCodeFromHttpStatus(status), Code::kUnknown);
}
TEST(GrpcProtocol, FragmentedMessagesAndCompressionWithoutConnectTerminal) {
  auto frame = EncodeStreamingMessage(std::string(1000, 'x'), true);
  ASSERT_TRUE(frame.ok());
  for (size_t split = 0; split <= frame.value()->size(); ++split) {
    StreamingDecoder decoder(true, true, {}, Protocol::kGrpc);
    std::vector<std::string> messages;
    for (std::string_view part :
         {std::string_view(*frame.value()).substr(0, split),
          std::string_view(*frame.value()).substr(split)}) {
      while (!part.empty()) {
        auto value = decoder.ReadOne(part);
        ASSERT_TRUE(value.ok());
        if (*value.value()) messages.push_back(std::move(**value.value()));
      }
    }
    EXPECT_EQ(messages, (std::vector<std::string>{std::string(1000, 'x')}));
    EXPECT_TRUE(
        decoder.Finish().ok());  // gRPC status is separately in HTTP trailers.
  }
  auto terminal = EncodeEndStream({});
  std::string_view bytes = *terminal.value();
  StreamingDecoder decoder(true, false, {}, Protocol::kGrpc);
  EXPECT_FALSE(decoder.ReadOne(bytes).ok());
}
}  // namespace
}  // namespace connectrpc
