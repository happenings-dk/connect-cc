#include "protocol_json.h"

#include "connectrpc/unary.h"
#include "gtest/gtest.h"
namespace connectrpc {
namespace {
TEST(ProtocolJson, RejectsMalformedUtf8NumbersDuplicatesAndDeepNesting) {
  for (const auto& bytes :
       {std::string("{\"x\":1,\"x\":2}"), std::string("01"), std::string("1."),
        std::string("[1,]"), std::string("{\"a\":true,}"),
        std::string("\"\\ud800\""), std::string("\"\\udc00\""),
        std::string("\"\xc0\x80\""), std::string("\"\xed\xa0\x80\""),
        std::string(66, '[') + "0" + std::string(66, ']')}) {
    EXPECT_FALSE(internal_json::Parse(bytes).ok()) << bytes;
  }
}
TEST(ProtocolJson, UnicodeEscapesAndUnknownFieldsRoundTrip) {
  auto value = internal_json::Parse(
      R"({"message":"\ud83d\ude00\n\u0000","unknown":[true,false,null,-123.4e+50]})");
  ASSERT_TRUE(value.ok());
  auto encoded = internal_json::Encode(*value.value());
  ASSERT_TRUE(encoded.ok());
  auto decoded = DecodeError(*encoded.value());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value()->status.message, std::string("😀\n\0", 6));
}
TEST(ProtocolJson, AllStatusCodesAndBinaryDetails) {
  for (int code = 1; code < 17; ++code) {
    Status status{static_cast<Code>(code),
                  "quotes \" and slash \\ and unicode 你好",
                  {{"example.Detail", std::string("\0\xff", 2)}}};
    auto encoded = EncodeError(status);
    ASSERT_TRUE(encoded.ok());
    auto decoded = DecodeError(*encoded.value());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value()->status.code, status.code);
    EXPECT_EQ(decoded.value()->status.message, status.message);
    ASSERT_EQ(decoded.value()->status.details.size(), 1);
    EXPECT_EQ(decoded.value()->status.details[0].value,
              status.details[0].value);
  }
}
}  // namespace
}  // namespace connectrpc
