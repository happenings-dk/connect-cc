#include "connectrpc/http1.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace connectrpc {
namespace {
struct Parsed {
  Metadata headers;
  Metadata trailers;
  std::string body;
  bool keep_alive = false;
};
Result<Parsed> ReadMessage(Http1Parser& parser, std::string_view& wire,
                           size_t chunk) {
  Parsed output;
  size_t attempts = 2 * wire.size() + 10;
  for (;;) {
    if (attempts-- == 0)
      return Result<Parsed>(
          Status{Code::kInternal, "parser made no progress", {}});
    std::string_view bytes = wire.substr(0, chunk);
    size_t before = bytes.size();
    auto event = parser.ReadOne(bytes);
    wire.remove_prefix(before - bytes.size());
    if (!event.ok()) return Result<Parsed>(event.status());
    if (!*event.value()) {
      if (!wire.empty()) continue;
      event = parser.EndInput();
      if (!event.ok()) return Result<Parsed>(event.status());
      if (!*event.value())
        return Result<Parsed>(Status{Code::kInternal, "missing end event", {}});
    }
    auto& next = **event.value();
    switch (next.kind) {
      case Http1Event::Kind::kHeaders:
        output.headers = std::move(next.headers);
        output.keep_alive = next.keep_alive;
        break;
      case Http1Event::Kind::kBody:
        EXPECT_FALSE(next.body.empty());
        EXPECT_LE(next.body.size(), chunk);
        output.body += next.body;
        break;
      case Http1Event::Kind::kEnd:
        output.trailers = std::move(next.headers);
        return Result<Parsed>(std::move(output));
    }
  }
}
Result<Parsed> Parse(std::string_view wire, Http1Role role = Http1Role::kServer,
                     size_t chunk = 16384, Http1Limits limits = {}) {
  auto parser = Http1Parser::Create(role, limits);
  if (!parser.ok()) return Result<Parsed>(parser.status());
  return ReadMessage(**parser.value(), wire, chunk);
}
TEST(Http1, PreservesUnrecognizedResponseStatus) {
  auto parsed = Parse("HTTP/1.1 800 Custom\r\ncontent-length: 0\r\n\r\n",
                      Http1Role::kClient);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message;
  EXPECT_EQ(parsed.value()->headers,
            (Metadata{{":status", "800"}, {"content-length", "0"}}));
}
TEST(Http1, FragmentedRequestsWithChunkExtensionsAndTrailers) {
  const std::string wire =
      "POST /example.Service/Call?x=y HTTP/1.1\r\nHost: "
      "localhost:8080\r\nContent-Type: application/proto\r\nTransfer-Encoding: "
      "chunked\r\nX-Custom: a\r\nX-Custom: "
      "b\r\n\r\n3;foo=bar\r\nabc\r\n4;foo=\"x y\"\r\ndefg\r\n0\r\nX-Cost: "
      "1\r\nX-Cost: 2\r\n\r\n";
  for (size_t chunk = 1; chunk <= wire.size(); ++chunk) {
    SCOPED_TRACE(chunk);
    auto parsed = Parse(wire, Http1Role::kServer, chunk);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message;
    EXPECT_EQ(parsed.value()->headers,
              (Metadata{{":method", "POST"},
                        {":path", "/example.Service/Call?x=y"},
                        {":authority", "localhost:8080"},
                        {"content-type", "application/proto"},
                        {"x-custom", "a"},
                        {"x-custom", "b"}}));
    EXPECT_EQ(parsed.value()->body, "abcdefg");
    EXPECT_EQ(parsed.value()->trailers,
              (Metadata{{"x-cost", "1"}, {"x-cost", "2"}}));
    EXPECT_TRUE(parsed.value()->keep_alive);
  }
}
TEST(Http1, ContentLengthAndEmptyBodiesAtEveryFragmentSize) {
  for (std::string body : {std::string(), std::string("\0\1\r\n\xff", 5)}) {
    std::string wire =
        "POST /call HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    for (size_t chunk = 1; chunk <= wire.size(); ++chunk) {
      auto parsed = Parse(wire, Http1Role::kServer, chunk);
      ASSERT_TRUE(parsed.ok()) << parsed.status().message;
      EXPECT_EQ(parsed.value()->body, body);
      EXPECT_TRUE(parsed.value()->keep_alive);
    }
  }
  EXPECT_TRUE(Parse("GET /call HTTP/1.1\r\nHost: x\r\n\r\n").ok());
  EXPECT_TRUE(Parse("OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n").ok());
}
TEST(Http1, AbsoluteTargetsNormalizeAuthorityAndEmptyFieldsSurvive) {
  auto origin =
      Parse("GET /call?url=http://example.com HTTP/1.1\r\nHost: x\r\n\r\n");
  ASSERT_TRUE(origin.ok());
  EXPECT_EQ(origin.value()->headers[1].second, "/call?url=http://example.com");

  for (const auto& [uri, path] :
       std::vector<std::pair<std::string, std::string>>{
           {"http://example.com/call", "/call"},
           {"HTTP://example.com?x=y", "/?x=y"},
           {"https://example.com", "/"}}) {
    auto parsed = Parse(
        "GET " + uri + " HTTP/1.1\r\nHost: ignored.example\r\nX-Empty:\r\n\r\n",
        Http1Role::kServer, 1);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message;
    EXPECT_EQ(parsed.value()->headers[1],
              (std::pair<std::string, std::string>{":path", path}));
    EXPECT_EQ(parsed.value()->headers[2], (std::pair<std::string, std::string>{
                                              ":authority", "example.com"}));
    EXPECT_EQ(parsed.value()->headers.back(),
              (std::pair<std::string, std::string>{"x-empty", ""}));
  }
}
TEST(Http1, KeepsPipelinedMessagesUnconsumedUntilAdmitted) {
  auto parser = Http1Parser::Create(Http1Role::kServer);
  ASSERT_TRUE(parser.ok());
  const std::string first =
      "POST /one HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\na";
  const std::string second =
      "POST /two HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nConnection: "
      "close\r\n\r\nb";
  std::string pipeline = first + second;
  std::string_view wire = pipeline;
  auto one = ReadMessage(**parser.value(), wire, pipeline.size());
  ASSERT_TRUE(one.ok()) << one.status().message;
  EXPECT_EQ(wire, second);
  EXPECT_EQ(one.value()->body, "a");
  EXPECT_FALSE((*parser.value())->ReadOne(wire).ok());
  EXPECT_EQ(wire, second);
  ASSERT_TRUE((*parser.value())->NextMessage().ok());
  auto two = ReadMessage(**parser.value(), wire, pipeline.size());
  ASSERT_TRUE(two.ok());
  EXPECT_EQ(two.value()->body, "b");
  EXPECT_TRUE(wire.empty());
  EXPECT_FALSE(two.value()->keep_alive);
  EXPECT_FALSE((*parser.value())->NextMessage().ok());
}
TEST(Http1, ResponseLengthsInformationalAndCloseDelimitedEof) {
  for (size_t chunk = 1; chunk < 90; ++chunk) {
    auto parsed =
        Parse("HTTP/1.1 200 OK\r\nContent-Type: application/proto\r\n\r\nhello",
              Http1Role::kClient, chunk);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message;
    EXPECT_EQ(parsed.value()->body, "hello");
    EXPECT_FALSE(parsed.value()->keep_alive);
    auto empty =
        Parse("HTTP/1.1 204 No Content\r\n\r\n", Http1Role::kClient, chunk);
    ASSERT_TRUE(empty.ok()) << empty.status().message;
    EXPECT_TRUE(empty.value()->body.empty());
  }
  for (std::string_view bad :
       {"HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 100 Continue\r\nTransfer-Encoding: chunked\r\n\r\n"})
    EXPECT_FALSE(Parse(bad, Http1Role::kClient).ok());
  auto parser = Http1Parser::Create(Http1Role::kClient);
  ASSERT_TRUE(parser.ok());
  std::string_view wire =
      "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: "
      "1\r\n\r\nx";
  auto info = ReadMessage(**parser.value(), wire, 1000);
  ASSERT_TRUE(info.ok());
  EXPECT_EQ(info.value()->headers, (Metadata{{":status", "100"}}));
  ASSERT_TRUE((*parser.value())->NextMessage().ok());
  auto final = ReadMessage(**parser.value(), wire, 1000);
  ASSERT_TRUE(final.ok());
  EXPECT_EQ(final.value()->body, "x");
}
TEST(Http1, RejectsAmbiguousFramingInvalidAuthorityAndUpgrade) {
  for (std::string headers :
       {"Content-Length: 1\r\nContent-Length: 2\r\n",
        "Content-Length: 1\r\nContent-Length: 1\r\n",
        "Content-Length: 1, 1\r\n", "Content-Length: -1\r\n",
        "Content-Length: 18446744073709551616\r\n",
        "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n",
        "Transfer-Encoding: gzip, chunked\r\n",
        "Transfer-Encoding: chunked, gzip\r\n",
        "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n",
        "Host: duplicate\r\n", "Connection: content-length\r\n",
        "Connection: upgrade\r\nUpgrade: websocket\r\n", "Bad Name: value\r\n",
        "Content-Length: 0\r\n folded\r\n"}) {
    SCOPED_TRACE(headers);
    EXPECT_FALSE(
        Parse("POST /call HTTP/1.1\r\nHost: x\r\n" + headers + "\r\n").ok());
  }
  for (std::string_view wire :
       {"POST /call HTTP/1.1\r\n\r\n", "POST /call HTTP/1.1\r\nHost:\r\n\r\n",
        "POST /call HTTP/1.0\r\nHost: x\r\n\r\n",
        "GET http://user@example.com/call HTTP/1.1\r\nHost: x\r\n\r\n",
        "POST /call#fragment HTTP/1.1\r\nHost: x\r\n\r\n",
        "CONNECT example.com:443 HTTP/1.1\r\nHost: x\r\n\r\n"})
    EXPECT_FALSE(Parse(wire).ok()) << wire;
}
TEST(Http1, RejectsEmptyTransferEncodingAcrossParserVersions) {
  for (Http1Role role : {Http1Role::kClient, Http1Role::kServer}) {
    for (std::string_view value : {"", " ", "\t"}) {
      std::string wire = role == Http1Role::kClient
                             ? "HTTP/1.1 200 OK\r\n"
                             : "POST /call HTTP/1.1\r\nHost: x\r\n";
      wire.append("Transfer-Encoding:").append(value).append("\r\n\r\n");
      for (size_t chunk : {size_t{1}, size_t{7}, size_t{16384}}) {
        SCOPED_TRACE(wire);
        SCOPED_TRACE(chunk);
        EXPECT_FALSE(Parse(wire, role, chunk).ok());
      }
    }
  }
}
TEST(Http1, RejectsTruncationForbiddenTrailersAndInvalidChunks) {
  for (std::string_view tail :
       {"", "0\r\n", "1\r\nx", "3\r\nxy", "g\r\nx\r\n0\r\n\r\n",
        "1\r\nxX0\r\n\r\n", "0\r\nContent-Length: 0\r\n\r\n",
        "0\r\nHost: bad\r\n\r\n", "0\r\nX-Hop: bad\r\n\r\n"}) {
    std::string wire =
        "POST /call HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: "
        "chunked\r\nConnection: x-hop\r\n\r\n" +
        std::string(tail);
    EXPECT_FALSE(Parse(wire).ok()) << tail;
  }
  EXPECT_FALSE(
      Parse("POST /call HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nx")
          .ok());
  auto parsed = Parse(
      "POST /call HTTP/1.1\r\nHost: x\r\nConnection: x-hop\r\nX-Hop: "
      "hidden\r\nX-End: visible\r\n\r\n");
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(parsed.value()->headers.back(),
            (std::pair<std::string, std::string>{"x-end", "visible"}));
  EXPECT_EQ(parsed.value()->headers.size(), 4);
}
TEST(Http1, BoundsMetadataFramingAndBodyChunks) {
  Http1Limits limits{128, 7};
  for (std::string wire :
       {"POST /" + std::string(200, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n",
        "POST /call HTTP/1.1\r\nHost: x\r\nX-Long: " + std::string(200, 'x') +
            "\r\n\r\n",
        "POST /call HTTP/1.1\r\nHost: " + std::string(200, ' ') + "x\r\n\r\n",
        "POST /call HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: "
        "chunked\r\n\r\n1;long=" +
            std::string(200, 'x') + "\r\nx\r\n0\r\n\r\n",
        "POST /call HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: "
        "chunked\r\n\r\n0\r\nX-Long: " +
            std::string(200, 'x') + "\r\n\r\n"}) {
    auto parsed = Parse(wire, Http1Role::kServer, 7, limits);
    EXPECT_EQ(parsed.status().code, Code::kResourceExhausted)
        << parsed.status().message;
  }
  std::string large =
      "POST /call HTTP/1.1\r\nHost: x\r\nContent-Length: 10000\r\n\r\n" +
      std::string(10000, 'x');
  auto parsed = Parse(large, Http1Role::kServer, 7, limits);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message;
  EXPECT_EQ(parsed.value()->body.size(), 10000);
  auto parser = Http1Parser::Create(Http1Role::kServer);
  ASSERT_TRUE(parser.ok());
  std::string_view bad = "INVALID HTTP\r\n";
  EXPECT_FALSE((*parser.value())->ReadOne(bad).ok());
  bad = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  EXPECT_FALSE((*parser.value())->ReadOne(bad).ok());
  EXPECT_FALSE((*parser.value())->NextMessage().ok());
  EXPECT_FALSE(Http1Parser::Create(Http1Role::kServer, {0, 1}).ok());
}
TEST(Http1, WriterRoundTripsChunkedAndFixedBodiesWithReuseGuard) {
  for (Http1Role role : {Http1Role::kClient, Http1Role::kServer}) {
    for (bool fixed : {false, true}) {
      Http1Encoder encoder(role);
      Metadata headers = role == Http1Role::kClient
                             ? Metadata{{":method", "POST"},
                                        {":path", "/call"},
                                        {":authority", "example.com"}}
                             : Metadata{{":status", "200"}};
      headers.emplace_back("content-type", "application/proto");
      if (fixed) headers.emplace_back("content-length", "6");
      auto head = encoder.Headers(headers, !fixed);
      ASSERT_TRUE(head.ok()) << head.status().message;
      EXPECT_FALSE(encoder.Headers(headers).ok());
      std::string wire = *head.value();
      for (std::string_view body : {"", "abc", "", "def"}) {
        auto encoded = encoder.Body(body);
        ASSERT_TRUE(encoded.ok());
        wire += *encoded.value();
      }
      Metadata trailers =
          fixed ? Metadata{} : Metadata{{"cost", "1"}, {"cost", "2"}};
      auto end = encoder.End(trailers);
      ASSERT_TRUE(end.ok());
      wire += *end.value();
      EXPECT_FALSE(encoder.Body("late").ok());
      EXPECT_FALSE(encoder.End().ok());
      auto parsed = Parse(
          wire,
          role == Http1Role::kClient ? Http1Role::kServer : Http1Role::kClient,
          1);
      ASSERT_TRUE(parsed.ok()) << parsed.status().message;
      EXPECT_EQ(parsed.value()->body, "abcdef");
      EXPECT_EQ(parsed.value()->trailers, trailers);
      EXPECT_EQ(parsed.value()->keep_alive, !fixed);
    }
  }
}
TEST(Http1, WriterRejectsInvalidHeadersLengthsAndTrailers) {
  Http1Encoder limited(Http1Role::kClient, {128, 3});
  EXPECT_EQ(limited
                .Headers({{":method", "POST"},
                          {":path", "/" + std::string(200, 'x')},
                          {":authority", "x"}})
                .status()
                .code,
            Code::kResourceExhausted);
  Http1Encoder body_limited(Http1Role::kServer, {128, 3});
  ASSERT_TRUE(body_limited.Headers({{":status", "200"}}).ok());
  EXPECT_EQ(body_limited.Body("four").status().code, Code::kResourceExhausted);
  EXPECT_EQ(body_limited.End({{"cost", std::string(200, 'x')}}).status().code,
            Code::kResourceExhausted);

  Metadata headers{
      {":method", "POST"}, {":path", "/call"}, {":authority", "x"}};
  for (const auto& field : Metadata{{"content-length", ""},
                                    {"content-length", "-1"},
                                    {"transfer-encoding", "chunked"},
                                    {"x-header", "bad\r\ninjection: yes"},
                                    {"Upper", "no"},
                                    {":status", "200"},
                                    {":path", "/duplicate"},
                                    {"host", "other"}}) {
    Http1Encoder encoder(Http1Role::kClient);
    auto bad = headers;
    bad.push_back(field);
    EXPECT_FALSE(encoder.Headers(bad).ok()) << field.first;
  }
  Http1Encoder fixed(Http1Role::kClient);
  headers.emplace_back("content-length", "2");
  ASSERT_TRUE(fixed.Headers(headers).ok());
  EXPECT_FALSE(fixed.End().ok());
  EXPECT_FALSE(fixed.Body("long").ok());
  EXPECT_TRUE(fixed.Body("ok").ok());
  EXPECT_FALSE(fixed.End({{"cost", "1"}}).ok());
  EXPECT_TRUE(fixed.End().ok());
  Http1Encoder chunked(Http1Role::kServer);
  ASSERT_TRUE(chunked.Headers({{":status", "200"}}).ok());
  EXPECT_FALSE(chunked.End({{"content-length", "0"}}).ok());
  EXPECT_TRUE(chunked.End({{"cost", "1"}}).ok());
  Http1Encoder empty(Http1Role::kServer);
  ASSERT_TRUE(empty.Headers({{":status", "204"}}).ok());
  EXPECT_FALSE(empty.Body("x").ok());
  EXPECT_TRUE(empty.End().ok());
  Http1Encoder get(Http1Role::kClient);
  auto get_headers = get.Headers(
      {{":method", "GET"}, {":path", "/call?connect=v1"}, {":authority", "x"}});
  ASSERT_TRUE(get_headers.ok());
  EXPECT_EQ(get_headers.value()->find("transfer-encoding"), std::string::npos);
  EXPECT_FALSE(get.Body("x").ok());
  EXPECT_TRUE(get.End().ok());
}
}  // namespace
}  // namespace connectrpc
