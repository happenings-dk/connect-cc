#include "connectrpc/codec.h"
#include "connectrpc/envelope.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"
namespace connectrpc {
namespace {
TEST(Envelope, EverySplitBoundary) {
  for (Protocol protocol :
       {Protocol::kConnect, Protocol::kGrpc, Protocol::kGrpcWeb}) {
    auto encoded = EncodeEnvelope({false, false, "hello"}, protocol);
    ASSERT_TRUE(encoded.ok());
    for (size_t split = 0; split <= encoded.value()->size(); ++split) {
      EnvelopeDecoder decoder(protocol, 5);
      int count = 0;
      auto sink = [&count](Envelope envelope) {
        ++count;
        EXPECT_EQ(envelope.payload, "hello");
        return Status{};
      };
      EXPECT_TRUE(
          decoder
              .Feed(std::string_view(*encoded.value()).substr(0, split), sink)
              .ok());
      EXPECT_TRUE(
          decoder.Feed(std::string_view(*encoded.value()).substr(split), sink)
              .ok());
      EXPECT_TRUE(decoder.Finish().ok());
      EXPECT_EQ(count, 1);
    }
  }
}
TEST(Envelope, EmptyAndCoalesced) {
  auto a = EncodeEnvelope({false, false, ""}, Protocol::kConnect);
  auto b = EncodeEnvelope({false, true, "{}"}, Protocol::kConnect);
  EnvelopeDecoder decoder(Protocol::kConnect, 10);
  int count = 0;
  EXPECT_TRUE(decoder
                  .Feed(*a.value() + *b.value(),
                        [&count](Envelope) {
                          ++count;
                          return Status{};
                        })
                  .ok());
  EXPECT_EQ(count, 2);
  EXPECT_TRUE(decoder.Finish().ok());
  EXPECT_FALSE(decoder.Feed("x", [](Envelope) { return Status{}; }).ok());
}
TEST(Envelope, RejectsInvalidFlagsAndLengthBeforeBody) {
  for (std::string bytes :
       {std::string("\x04\x03\0\0\0\0\x03\0\0\0\0\x03\0\0\0\0\x03\0\0\0\0", 5),
        std::string("\0\xff\xff\xff\xff", 5)}) {
    EnvelopeDecoder decoder(Protocol::kConnect, 16);
    EXPECT_FALSE(decoder
                     .Feed(bytes,
                           [](Envelope) {
                             ADD_FAILURE();
                             return Status{};
                           })
                     .ok());
    EXPECT_FALSE(decoder.Finish().ok());
  }
}
TEST(Envelope, TruncationAndSinkFailure) {
  EnvelopeDecoder decoder(Protocol::kGrpc, 16);
  EXPECT_TRUE(decoder
                  .Feed(std::string("\0\0\0\0\x02x", 6),
                        [](Envelope) { return Status{}; })
                  .ok());
  EXPECT_EQ(decoder.Finish().code, Code::kDataLoss);
  EXPECT_EQ(
      decoder
          .Feed("y",
                [](Envelope) { return Status{Code::kCanceled, "stopped", {}}; })
          .code,
      Code::kCanceled);
  EXPECT_EQ(decoder.Finish().code, Code::kCanceled);
}
TEST(Codec, BinaryAndJsonRoundTrip) {
  google::protobuf::StringValue input;
  input.set_value("hello 🌍");
  for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
    auto bytes = EncodeMessage(input, codec);
    ASSERT_TRUE(bytes.ok());
    google::protobuf::StringValue output;
    ASSERT_TRUE(DecodeMessage(*bytes.value(), codec, output).ok());
    EXPECT_EQ(output.value(), input.value());
  }
}
TEST(Codec, GzipBoundsTruncationAndTrailingData) {
  for (std::string input : {std::string(), std::string(100000, 'a')}) {
    auto encoded = Gzip(input);
    ASSERT_TRUE(encoded.ok());
    auto decoded = Gunzip(*encoded.value(), input.size());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(*decoded.value(), input);
    EXPECT_FALSE(
        Gunzip(encoded.value()->substr(0, encoded.value()->size() - 1), 100000)
            .ok());
    EXPECT_FALSE(Gunzip(*encoded.value() + "x", 100000).ok());
    if (!input.empty()) {
      EXPECT_EQ(Gunzip(*encoded.value(), input.size() - 1).status().code,
                Code::kResourceExhausted);
    }
  }
}
TEST(Status, EmptySuccessCannotCreateValuelessResult) {
  Result<int> result(Status{});
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.value(), nullptr);
  EXPECT_EQ(result.status().code, Code::kInternal);
}
TEST(Codec, GzipMembersShareOneOutputLimit) {
  auto empty = Gzip("");
  auto first = Gzip(std::string(16384, 'a'));
  auto second = Gzip(std::string(16385, 'b'));
  ASSERT_TRUE(empty.ok());
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  const std::string encoded = *empty.value() + *first.value() + *empty.value() +
                              *second.value() + *empty.value();
  auto decoded = Gunzip(encoded, 32769);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message;
  EXPECT_EQ(*decoded.value(),
            std::string(16384, 'a') + std::string(16385, 'b'));
  EXPECT_EQ(Gunzip(encoded, 32768).status().code, Code::kResourceExhausted);
  auto empty_members = Gunzip(*empty.value() + *empty.value(), 0);
  ASSERT_TRUE(empty_members.ok());
  EXPECT_TRUE(empty_members.value()->empty());
}
TEST(Codec, GzipRejectsInvalidLaterMember) {
  auto first = Gzip("first");
  auto second = Gzip("second");
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  for (size_t length = 1; length < second.value()->size(); ++length) {
    EXPECT_EQ(Gunzip(*first.value() + second.value()->substr(0, length), 100)
                  .status()
                  .code,
              Code::kInvalidArgument);
  }
  std::string corrupt = *second.value();
  corrupt[corrupt.size() - 8] ^= 1;  // Corrupt the second member's CRC32.
  EXPECT_EQ(Gunzip(*first.value() + corrupt, 100).status().code,
            Code::kInvalidArgument);
  EXPECT_EQ(
      Gunzip(*first.value() + *second.value() + "junk", 100).status().code,
      Code::kInvalidArgument);
}
}  // namespace
}  // namespace connectrpc
