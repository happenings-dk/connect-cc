#include "connectrpc/cors.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "connectrpc/http1_connection.h"
#include "connectrpc/http2_connection.h"
#include "connectrpc/rpc.h"
#include "connectrpc/tls.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
std::string Header(const Metadata& headers, std::string_view name) {
  std::string result;
  for (const auto& [key, value] : headers) {
    if (key != name) continue;
    if (!result.empty()) result.append(", ");
    result.append(value);
  }
  return result;
}
CorsOptions Options() {
  CorsOptions options;
  options.allowed_origins = {"https://web.example",
                             "https://other.example:8443"};
  options.allowed_headers = {"Authorization", "X-Request-ID"};
  options.exposed_headers = {"X-Reply", "Trailer-X-Done"};
  options.allow_credentials = true;
  return options;
}
Metadata PreflightHeaders() {
  return {{":method", "OPTIONS"},
          {":path", "/test.Service/Echo"},
          {"origin", "https://web.example"},
          {"access-control-request-method", "POST"},
          {"access-control-request-headers",
           "Content-Type, Connect-Protocol-Version, Authorization"}};
}
TEST(Cors, PreflightAndProtocolHeaders) {
  auto policy = CorsPolicy::Create(Options());
  ASSERT_TRUE(policy.ok());
  auto response = policy.value()->Preflight(PreflightHeaders());
  ASSERT_TRUE(response);
  EXPECT_TRUE(response->body.empty());
  EXPECT_EQ(Header(response->headers, ":status"), "204");
  EXPECT_EQ(Header(response->headers, "access-control-allow-origin"),
            "https://web.example");
  EXPECT_EQ(Header(response->headers, "access-control-allow-methods"),
            "GET, POST");
  EXPECT_EQ(Header(response->headers, "access-control-allow-credentials"),
            "true");
  EXPECT_EQ(Header(response->headers, "access-control-max-age"), "7200");
  EXPECT_EQ(Header(response->headers, "vary"),
            "origin, access-control-request-method, "
            "access-control-request-headers");
  auto headers = PreflightHeaders();
  headers.back().second =
      "x-grpc-web, x-user-agent, grpc-timeout, connect-timeout-ms, "
      "connect-content-encoding, connect-accept-encoding, content-encoding, "
      "grpc-encoding, grpc-accept-encoding, X-Request-ID";
  EXPECT_EQ(Header(policy.value()->Preflight(headers)->headers, ":status"),
            "204");
  headers[3].second = "GET";
  headers.pop_back();
  EXPECT_EQ(Header(policy.value()->Preflight(headers)->headers, ":status"),
            "204");
}
TEST(Cors, RejectsInvalidAndDisallowedPreflights) {
  auto policy = CorsPolicy::Create(Options());
  ASSERT_TRUE(policy.ok());
  for (const auto& [index, value] : std::vector<std::pair<int, std::string>>{
           {2, "https://web.example.evil"},
           {2, "null"},
           {2, ""},
           {2, "https://web.example, https://other.example:8443"},
           {3, "DELETE"},
           {3, "post"},
           {3, "POST, GET"},
           {3, ""},
           {4, "x-secret"},
           {4, "content-type,"},
           {4, ",content-type"},
           {4, "x\r\ninjected"},
           {4, "*"}}) {
    auto request = PreflightHeaders();
    request[index].second = value;
    auto response = policy.value()->Preflight(request);
    ASSERT_TRUE(response);
    EXPECT_EQ(Header(response->headers, ":status"), "403") << value;
    EXPECT_TRUE(
        Header(response->headers, "access-control-allow-origin").empty());
    EXPECT_FALSE(Header(response->headers, "vary").empty());
  }
  for (std::string name : {"origin", "access-control-request-method"}) {
    auto request = PreflightHeaders();
    request.emplace_back(name, Header(request, name));
    EXPECT_EQ(Header(policy.value()->Preflight(request)->headers, ":status"),
              "403");
  }
  auto request = PreflightHeaders();
  request.emplace_back("access-control-request-headers", "x-request-id");
  EXPECT_EQ(Header(policy.value()->Preflight(request)->headers, ":status"),
            "204");
  request.back().second = "x-secret";
  EXPECT_EQ(Header(policy.value()->Preflight(request)->headers, ":status"),
            "403");
  auto defaults = CorsPolicy::Create({});
  EXPECT_EQ(Header(defaults.value()->Preflight(PreflightHeaders())->headers,
                   ":status"),
            "403");
}
TEST(Cors, ActualResponsesReplaceControlFieldsAndPreserveVary) {
  auto policy = CorsPolicy::Create(Options());
  ASSERT_TRUE(policy.ok());
  Metadata request{{"origin", "https://web.example"}};
  Metadata response{{":status", "500"},
                    {"vary", "Accept-Encoding"},
                    {"x-reply", "reply"},
                    {"trailer-x-done", "done"},
                    {"access-control-allow-origin", "*"},
                    {"access-control-expose-headers", "*"},
                    {"access-control-allow-private-network", "true"}};
  policy.value()->ApplyResponseHeaders(request, response);
  EXPECT_EQ(Header(response, "access-control-allow-origin"),
            "https://web.example");
  EXPECT_EQ(Header(response, "access-control-allow-credentials"), "true");
  EXPECT_EQ(Header(response, "vary"), "Accept-Encoding, origin");
  EXPECT_TRUE(Header(response, "access-control-allow-private-network").empty());
  for (std::string_view name : {"x-reply", "trailer-x-done", "grpc-status",
                                "grpc-message", "grpc-status-details-bin"})
    EXPECT_NE(Header(response, "access-control-expose-headers").find(name),
              std::string::npos);
  policy.value()->ApplyResponseHeaders(request, response);
  EXPECT_EQ(Header(response, "vary"), "Accept-Encoding, origin");
  request.emplace_back("origin", "https://web.example");
  policy.value()->ApplyResponseHeaders(request, response);
  EXPECT_TRUE(Header(response, "access-control-allow-origin").empty());
  EXPECT_TRUE(Header(response, "access-control-allow-credentials").empty());
  EXPECT_TRUE(Header(response, "access-control-expose-headers").empty());
  response = {{"vary", "*"}};
  policy.value()->ApplyResponseHeaders({}, response);
  EXPECT_EQ(Header(response, "vary"), "*");
  response = {{"vary", "Accept-Encoding, Origin"}};
  policy.value()->ApplyResponseHeaders({}, response);
  EXPECT_EQ(Header(response, "vary"), "Accept-Encoding, Origin");
}
TEST(Cors, OrdinaryRequestsAndExplicitCredentials) {
  CorsOptions options = Options();
  options.allow_credentials = false;
  options.max_age = std::chrono::seconds(0);
  options.allowed_origins.push_back("null");
  auto policy = CorsPolicy::Create(options);
  ASSERT_TRUE(policy.ok());
  EXPECT_FALSE(policy.value()->Preflight({}));
  EXPECT_FALSE(policy.value()->Preflight({{":method", "OPTIONS"}}));
  EXPECT_FALSE(policy.value()->Preflight(
      {{":method", "OPTIONS"}, {"origin", "https://web.example"}}));
  Metadata request = PreflightHeaders();
  request[0].second = "GET";
  EXPECT_FALSE(policy.value()->Preflight(request));
  Metadata response;
  policy.value()->ApplyResponseHeaders({{"origin", "null"}}, response);
  EXPECT_EQ(Header(response, "access-control-allow-origin"), "null");
  EXPECT_TRUE(Header(response, "access-control-allow-credentials").empty());
  auto preflight = policy.value()->Preflight(PreflightHeaders());
  EXPECT_TRUE(
      Header(preflight->headers, "access-control-allow-credentials").empty());
  EXPECT_EQ(Header(preflight->headers, "access-control-max-age"), "0");
  options.allowed_headers.clear();
  auto no_auth = CorsPolicy::Create(options);
  EXPECT_EQ(Header(no_auth.value()->Preflight(PreflightHeaders())->headers,
                   ":status"),
            "403");
}
TEST(Cors, ValidatesConfiguration) {
  for (std::string origin :
       {"*", "https://*.example", "", "https://example/",
        "https://user@example", "https://example?x", "https://example#x",
        "https://a\r\nx: y", "https://", "file://example"}) {
    CorsOptions options;
    options.allowed_origins = {origin};
    EXPECT_FALSE(CorsPolicy::Create(options).ok()) << origin;
  }
  for (std::string name : {"", "*", "x:bad", "x,other", "x\r\nother",
                           "Set-Cookie", "Access-Control-Allow-Origin"}) {
    CorsOptions options;
    options.allowed_headers = {name};
    EXPECT_FALSE(CorsPolicy::Create(options).ok()) << name;
    options.allowed_headers.clear();
    options.exposed_headers = {name};
    EXPECT_FALSE(CorsPolicy::Create(options).ok()) << name;
  }
  CorsOptions options;
  options.max_age = std::chrono::seconds(-1);
  EXPECT_FALSE(CorsPolicy::Create(options).ok());
  options.max_age = std::chrono::seconds(1);
  options.completion_timeout = std::chrono::milliseconds(0);
  EXPECT_FALSE(CorsPolicy::Create(options).ok());
}

template <typename Connection>
asio::awaitable<Result<std::shared_ptr<ByteStream>>> Accept(
    std::shared_ptr<Connection> connection) {
  auto accepted = co_await connection->Accept();
  if (!accepted.ok())
    co_return Result<std::shared_ptr<ByteStream>>(accepted.status());
  co_return Result<std::shared_ptr<ByteStream>>(*accepted.value());
}
template <typename Connection>
asio::awaitable<Result<std::shared_ptr<ByteStream>>> Open(
    std::shared_ptr<Connection> connection, Metadata headers,
    TransportOptions options) {
  headers.emplace(headers.begin(), ":authority", "localhost");
  headers.emplace(headers.begin(), ":scheme", "https");
  auto opened = co_await connection->Open(std::move(headers), options.deadline);
  if (!opened.ok())
    co_return Result<std::shared_ptr<ByteStream>>(opened.status());
  co_return Result<std::shared_ptr<ByteStream>>(*opened.value());
}

// Each scenario runs on HTTP/1.1 and HTTP/2, with and without TLS.
class CorsNetworkTest : public testing::TestWithParam<int> {
 protected:
  void SetUp() override {
    asio::error_code error;
    asio::ip::tcp::acceptor acceptor(loop_,
                                     {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket client(loop_);
    client.connect(acceptor.local_endpoint(), error);
    ASSERT_FALSE(error);
    asio::ip::tcp::socket server(loop_);
    acceptor.accept(server, error);
    ASSERT_FALSE(error);
    if (GetParam() & 2) {
      TlsOptions tls;
      tls.alpn = Http1() ? TlsAlpn::kHttp1 : TlsAlpn::kHttp2;
      tls.certificate_file = CONNECTCPP_TEST_CERT;
      tls.private_key_file = CONNECTCPP_TEST_KEY;
      ASSERT_TRUE(ConfigureTlsContext(server_tls_, true, tls).ok());
      tls = {};
      tls.alpn = Http1() ? TlsAlpn::kHttp1 : TlsAlpn::kHttp2;
      tls.trust_file = CONNECTCPP_TEST_CERT;
      ASSERT_TRUE(ConfigureTlsContext(client_tls_, false, tls).ok());
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    if (Http1()) {
      auto c =
          (GetParam() & 2)
              ? Http1Connection::CreateTls(
                    asio::ssl::stream<asio::ip::tcp::socket>(std::move(client),
                                                             client_tls_),
                    Http1Role::kClient, "localhost", {}, deadline)
              : Http1Connection::Create(std::move(client), Http1Role::kClient);
      auto s = (GetParam() & 2) ? Http1Connection::CreateTls(
                                      asio::ssl::stream<asio::ip::tcp::socket>(
                                          std::move(server), server_tls_),
                                      Http1Role::kServer, "", {}, deadline)
                                : Http1Connection::Create(std::move(server),
                                                          Http1Role::kServer);
      ASSERT_TRUE(c.ok());
      ASSERT_TRUE(s.ok());
      Connect(*c.value(), *s.value());
    } else {
      auto c = (GetParam() & 2)
                   ? Http2Connection::CreateTls(
                         asio::ssl::stream<asio::ip::tcp::socket>(
                             std::move(client), client_tls_),
                         Http2Session::Role::kClient, "localhost", {}, deadline)
                   : Http2Connection::Create(std::move(client),
                                             Http2Session::Role::kClient);
      auto s = (GetParam() & 2)
                   ? Http2Connection::CreateTls(
                         asio::ssl::stream<asio::ip::tcp::socket>(
                             std::move(server), server_tls_),
                         Http2Session::Role::kServer, "", {}, deadline)
                   : Http2Connection::Create(std::move(server),
                                             Http2Session::Role::kServer);
      ASSERT_TRUE(c.ok());
      ASSERT_TRUE(s.ok());
      Connect(*c.value(), *s.value());
    }
    watchdog_.expires_after(std::chrono::seconds(8));
    watchdog_.async_wait([this](asio::error_code error) {
      if (!error) {
        ADD_FAILURE() << "CORS connection stalled";
        loop_.stop();
      }
    });
  }
  template <typename Connection>
  void Connect(std::shared_ptr<Connection> client,
               std::shared_ptr<Connection> server) {
    exchange_ = [client](Metadata headers, TransportOptions options) {
      return Open(client, std::move(headers), options);
    };
    accept_ = [server] { return Accept(server); };
    cancel_client_ = [client] { return client->Cancel(); };
    cancel_server_ = [server] { return server->Cancel(); };
    for (auto connection : {client, server})
      asio::co_spawn(loop_, connection->Run(), [this](auto exception, Status) {
        EXPECT_FALSE(exception);
        if (++runs_done_ == 2) watchdog_.cancel();
      });
  }
  bool Http1() const { return !(GetParam() & 1); }
  void Serve(HttpStreamHandler handler) {
    asio::co_spawn(loop_, ServeLoop(std::move(handler)),
                   [](auto exception) { EXPECT_FALSE(exception); });
  }
  asio::awaitable<void> ServeLoop(HttpStreamHandler handler) {
    for (;;) {
      auto stream = co_await accept_();
      if (!stream.ok()) co_return;
      co_await handler(std::move(*stream.value()));
    }
  }
  asio::awaitable<HttpMessage> Request(HttpMessage request) {
    auto opened = co_await exchange_(std::move(request.headers), {});
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return HttpMessage{};
    auto stream = *opened.value();
    EXPECT_TRUE((co_await stream->Write(std::move(request.body))).ok());
    EXPECT_TRUE((co_await stream->CloseSend({})).ok());
    auto headers = co_await stream->ReadHeaders();
    EXPECT_TRUE(headers.ok()) << headers.status().message;
    HttpMessage response;
    if (!headers.ok()) co_return response;
    response.headers = std::move(*headers.value());
    for (;;) {
      auto bytes = co_await stream->Read();
      EXPECT_TRUE(bytes.ok());
      if (!bytes.ok() || !*bytes.value()) break;
      response.body.append(**bytes.value());
    }
    EXPECT_TRUE((co_await stream->Finish()).ok());
    co_return response;
  }
  void Run(asio::awaitable<void> operation) {
    asio::co_spawn(loop_, std::move(operation),
                   [](auto exception) { EXPECT_FALSE(exception); });
    loop_.run();
    EXPECT_EQ(runs_done_, 2);
  }
  asio::awaitable<void> Stop() {
    co_await cancel_client_();
    co_await cancel_server_();
  }
  asio::io_context loop_;
  asio::ssl::context client_tls_{asio::ssl::context::tls_client};
  asio::ssl::context server_tls_{asio::ssl::context::tls_server};
  asio::steady_timer watchdog_{loop_};
  StreamExchange exchange_;
  std::function<asio::awaitable<Result<std::shared_ptr<ByteStream>>>()> accept_;
  std::function<asio::awaitable<Status>()> cancel_client_;
  std::function<asio::awaitable<Status>()> cancel_server_;
  int runs_done_ = 0;
};

TEST_P(CorsNetworkTest, PreflightGetAndErrors) {
  using Message = google::protobuf::StringValue;
  Server server;
  int calls = 0;
  ASSERT_TRUE((server.RegisterUnary<Message, Message>(
                   "/test.Service/Echo",
                   [&](connectrpc::Request<Message> request)
                       -> asio::awaitable<Response<Message>> {
                     ++calls;
                     co_return Response<Message>{{},
                                                 request.message,
                                                 {{"x-reply", "reply"}},
                                                 {{"x-done", "done"}}};
                   },
                   {Idempotency::kNoSideEffects}))
                  .ok());
  auto policy = CorsPolicy::Create(Options());
  ASSERT_TRUE(policy.ok());
  Serve(policy.value()->Wrap([&](std::shared_ptr<ByteStream> stream) {
    return server.HandleHttpStream(std::move(stream));
  }));
  bool finished = false;
  auto invoke = [&]() -> asio::awaitable<void> {
    for (bool allowed : {true, false}) {
      HttpMessage request;
      request.headers = PreflightHeaders();
      if (!allowed) request.headers[2].second = "https://denied.example";
      auto response = co_await Request(std::move(request));
      EXPECT_EQ(Header(response.headers, ":status"), allowed ? "204" : "403");
      EXPECT_EQ(Header(response.headers, "access-control-allow-origin"),
                allowed ? "https://web.example" : "");
      EXPECT_TRUE(response.body.empty());
      EXPECT_EQ(calls, 0);
    }
    for (std::string origin :
         {"https://web.example", "https://denied.example", ""}) {
      CallOptions options;
      options.use_get = true;
      options.codec = Codec::kJson;
      auto request = EncodeUnaryRequest("/test.Service/Echo", "\"hello\"",
                                        options, {Idempotency::kNoSideEffects});
      EXPECT_TRUE(request.ok());
      if (!request.ok()) break;
      for (auto& [name, value] : request.value()->headers)
        if (name == "accept-encoding") value = "identity";
      if (!origin.empty())
        request.value()->headers.emplace_back("origin", origin);
      auto response = co_await Request(std::move(*request.value()));
      EXPECT_EQ(Header(response.headers, ":status"), "200");
      EXPECT_EQ(response.body, "\"hello\"");
      EXPECT_EQ(Header(response.headers, "x-reply"), "reply");
      EXPECT_EQ(Header(response.headers, "trailer-x-done"), "done");
      EXPECT_EQ(Header(response.headers, "access-control-allow-origin"),
                origin == "https://web.example" ? origin : "");
      EXPECT_EQ(Header(response.headers, "vary"), "origin");
    }
    EXPECT_EQ(calls, 3);  // CORS is not authorization.
    for (std::string method : {"GET", "OPTIONS", "DELETE"}) {
      HttpMessage request;
      request.headers = {{":method", method},
                         {":path", "/missing"},
                         {"origin", "https://web.example"}};
      auto response = co_await Request(std::move(request));
      EXPECT_NE(Header(response.headers, ":status"), "200");
      EXPECT_EQ(Header(response.headers, "access-control-allow-origin"),
                "https://web.example");
    }
    EXPECT_EQ(calls, 3);
    finished = true;
    co_await Stop();
  };
  Run(invoke());
  EXPECT_TRUE(finished);
}

TEST_P(CorsNetworkTest, RpcProtocolsAndShapes) {
  using Message = google::protobuf::StringValue;
  using Stream = TypedStream<Message, Message>;
  Server server;
  ASSERT_TRUE((server.RegisterUnary<Message, Message>(
                   "/test.Service/Echo",
                   [](connectrpc::Request<Message> request)
                       -> asio::awaitable<Response<Message>> {
                     co_return Response<Message>{{},
                                                 request.message,
                                                 {{"x-reply", "reply"}},
                                                 {{"x-done", "done"}}};
                   }))
                  .ok());
  ASSERT_TRUE(
      (server.RegisterServerStream<Message, Message>(
           "/test.Service/Expand",
           [](connectrpc::Request<Message> request,
              Stream stream) -> asio::awaitable<StreamCompletion> {
             EXPECT_TRUE(
                 (co_await stream.SendHeaders({{"x-reply", "reply"}})).ok());
             EXPECT_TRUE((co_await stream.Write(request.message)).ok());
             co_return StreamCompletion{
                 {Code::kPermissionDenied, "terminal error", {}},
                 {{"x-done", "done"}}};
           }))
          .ok());
  ASSERT_TRUE(
      (server.RegisterClientStream<Message, Message>(
           "/test.Service/Collect",
           [](CallContext,
              Stream stream) -> asio::awaitable<Response<Message>> {
             auto input = co_await stream.Read();
             EXPECT_TRUE(input.ok());
             Message message;
             if (input.ok() && *input.value()) message = **input.value();
             auto end = co_await stream.Read();
             EXPECT_TRUE(end.ok() && !*end.value());
             co_return Response<Message>{
                 {}, message, {{"x-reply", "reply"}}, {{"x-done", "done"}}};
           }))
          .ok());
  ASSERT_TRUE(
      (server.RegisterBidiStream<Message, Message>(
           "/test.Service/Chat",
           [](CallContext, Stream stream) -> asio::awaitable<StreamCompletion> {
             EXPECT_TRUE(
                 (co_await stream.SendHeaders({{"x-reply", "reply"}})).ok());
             auto input = co_await stream.Read();
             EXPECT_TRUE(input.ok());
             if (input.ok() && *input.value())
               EXPECT_TRUE((co_await stream.Write(**input.value())).ok());
             auto end = co_await stream.Read();
             EXPECT_TRUE(end.ok() && !*end.value());
             co_return StreamCompletion{{}, {{"x-done", "done"}}};
           }))
          .ok());
  auto policy = CorsPolicy::Create(Options());
  ASSERT_TRUE(policy.ok());
  Serve(policy.value()->Wrap([&](std::shared_ptr<ByteStream> stream) {
    EXPECT_EQ(stream->http_version(),
              Http1() ? HttpVersion::kHttp1 : HttpVersion::kHttp2);
    return server.HandleHttpStream(std::move(stream));
  }));
  bool finished = false;
  auto invoke = [&]() -> asio::awaitable<void> {
    Client client({}, exchange_);
    Message message;
    message.set_value("cors");
    for (Protocol protocol :
         {Protocol::kConnect, Protocol::kGrpc, Protocol::kGrpcWeb}) {
      if (Http1() && protocol == Protocol::kGrpc) continue;
      for (bool text : {false, true}) {
        if (text && protocol != Protocol::kGrpcWeb) continue;
        CallOptions unary;
        unary.protocol = protocol;
        unary.grpc_web_text = text;
        unary.headers = {{"origin", "https://web.example"}};
        auto response = co_await client.Unary<Message, Message>(
            "/test.Service/Echo", message, unary);
        EXPECT_TRUE(response.status.ok()) << response.status.message;
        EXPECT_EQ(Header(response.headers, "access-control-allow-origin"),
                  "https://web.example");
        for (int shape = 0; shape < (Http1() ? 2 : 3); ++shape) {
          StreamCallOptions options;
          options.protocol = protocol;
          options.grpc_web_text = text;
          options.headers = unary.headers;
          Result<Stream> opened;
          if (shape == 0)
            opened = co_await client.ServerStream<Message, Message>(
                "/test.Service/Expand", message, options);
          else if (shape == 1)
            opened = co_await client.ClientStream<Message, Message>(
                "/test.Service/Collect", options);
          else
            opened = co_await client.BidiStream<Message, Message>(
                "/test.Service/Chat", options);
          EXPECT_TRUE(opened.ok());
          if (!opened.ok()) continue;
          auto stream = std::move(*opened.value());
          if (shape != 0) {
            EXPECT_TRUE((co_await stream.Write(message)).ok());
            EXPECT_TRUE((co_await stream.CloseSend()).ok());
          }
          auto headers = co_await stream.ReadHeaders();
          EXPECT_TRUE(headers.ok());
          if (headers.ok()) {
            EXPECT_EQ(Header(*headers.value(), "access-control-allow-origin"),
                      "https://web.example");
            EXPECT_EQ(
                Header(*headers.value(), "access-control-allow-credentials"),
                "true");
            EXPECT_EQ(Header(*headers.value(), "x-reply"), "reply");
          }
          auto input = co_await stream.Read();
          EXPECT_TRUE(input.ok());
          if (input.ok() && *input.value())
            EXPECT_EQ((**input.value()).value(), "cors");
          auto end = co_await stream.Read();
          EXPECT_TRUE(end.ok() && !*end.value());
          auto completion = co_await stream.Finish();
          EXPECT_EQ(completion.status.code,
                    shape == 0 ? Code::kPermissionDenied : Code::kOk);
          EXPECT_EQ(Header(completion.trailers, "x-done"), "done");
        }
      }
    }
    finished = true;
    co_await Stop();
  };
  Run(invoke());
  EXPECT_TRUE(finished);
}
INSTANTIATE_TEST_SUITE_P(Http1Http2TcpTls, CorsNetworkTest,
                         testing::Values(0, 1, 2, 3));
}  // namespace
}  // namespace connectrpc
