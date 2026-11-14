#include "connectrpc/managed_transport.h"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>

#ifdef CONNECTCPP_PROTOBUF_LITE
#include "test/v1/lite.pb.h"
#else
#include "google/protobuf/wrappers.pb.h"
#endif
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
using namespace std::chrono_literals;
#ifdef CONNECTCPP_PROTOBUF_LITE
using Message = lite::v1::Message;
#else
using Message = google::protobuf::StringValue;
#endif
TEST(ManagedTransport, RejectsMalformedUrlsAndLimits) {
  asio::io_context loop;
  for (auto url :
       {"ftp://host", "https://", "http://user@host", "http://host:0",
        "http://host:65536", "http://host:", "http://host/path?q=x",
        "http://a\\b", "http://[broken]", "http://a\nb"})
    EXPECT_FALSE(ManagedTransport::Create(loop.get_executor(), url).ok())
        << url;
  for (auto url : {"http://localhost", "https://example.com:443/prefix",
                   "http://[::1]:80"})
    EXPECT_TRUE(ManagedTransport::Create(loop.get_executor(), url).ok()) << url;
  ManagedTransportOptions options;
  options.max_connections = 0;
  EXPECT_FALSE(
      ManagedTransport::Create(loop.get_executor(), "http://localhost", options)
          .ok());
  loop.poll();
}
class ManagedConnectionTest
    : public testing::TestWithParam<std::tuple<HttpVersion, bool>> {};
TEST_P(ManagedConnectionTest, ReuseIdleExpiryAndJoinedShutdown) {
  auto [version, secure] = GetParam();
  asio::io_context loop;
  asio::ip::tcp::acceptor listener(loop, {asio::ip::address_v4::loopback(), 0});
  asio::ssl::context server_tls(asio::ssl::context::tls_server);
  TlsOptions tls;
  tls.certificate_file = CONNECTCPP_TEST_CERT;
  tls.private_key_file = CONNECTCPP_TEST_KEY;
  tls.alpn = version == HttpVersion::kHttp1 ? TlsAlpn::kHttp1 : TlsAlpn::kHttp2;
  ASSERT_TRUE(ConfigureTlsContext(server_tls, true, tls).ok());
  ManagedTransportOptions options;
  options.cleartext_version = version;
  options.connection.alpn = tls.alpn;
  options.tls.trust_file = CONNECTCPP_TEST_CERT;
  options.idle_timeout = 40ms;
  options.max_connections = 1;
  auto created = ManagedTransport::Create(
      loop.get_executor(),
      std::string(secure ? "https" : "http") + "://localhost:" +
          std::to_string(listener.local_endpoint().port()) + "/prefix",
      options);
  ASSERT_TRUE(created.ok());
  auto transport = *created.value();
  auto client = transport->MakeClient();
  Server server;
  ASSERT_TRUE(
      (server.RegisterUnary<Message, Message>(
           "/prefix/test/Say",
           [](Request<Message> request) -> asio::awaitable<Response<Message>> {
             Response<Message> response;
             response.message = std::move(request.message);
             co_return response;
           }))
          .ok());
  ASSERT_TRUE(
      (server.RegisterServerStream<Message, Message>(
           "/prefix/test/Expand",
           [](Request<Message> request, TypedStream<Message, Message> stream)
               -> asio::awaitable<StreamCompletion> {
             co_return StreamCompletion{co_await stream.Write(request.message),
                                        {}};
           }))
          .ok());
  ASSERT_TRUE((server.RegisterClientStream<Message, Message>(
                   "/prefix/test/Collect",
                   [](CallContext, TypedStream<Message, Message> stream)
                       -> asio::awaitable<Response<Message>> {
                     Response<Message> response;
                     auto next = co_await stream.Read();
                     response.status = next.status();
                     if (next.ok()) response.message = std::move(*next.value());
                     auto end = co_await stream.Read();
                     EXPECT_TRUE(end.ok() && !*end.value());
                     co_return response;
                   }))
                  .ok());
  ASSERT_TRUE((server.RegisterBidiStream<Message, Message>(
                   "/prefix/test/Chat",
                   [](CallContext, TypedStream<Message, Message> stream)
                       -> asio::awaitable<StreamCompletion> {
                     for (;;) {
                       auto next = co_await stream.Read();
                       if (!next.ok())
                         co_return StreamCompletion{next.status(), {}};
                       if (!*next.value()) co_return StreamCompletion{};
                       auto sent = co_await stream.Write(**next.value());
                       if (!sent.ok()) co_return StreamCompletion{sent, {}};
                     }
                   }))
                  .ok());
  int accepted = 0, joined = 0, completed = 0;
  bool accept_joined = false;
  auto accept = [&]() -> asio::awaitable<void> {
    for (;;) {
      auto [error, socket] =
          co_await listener.async_accept(asio::as_tuple(asio::use_awaitable));
      if (error) co_return;
      ++accepted;
      Result<std::shared_ptr<HttpConnection>> connected;
      if (secure)
        connected = co_await HttpConnection::CreateTls(
            asio::ssl::stream<asio::ip::tcp::socket>(std::move(socket),
                                                     server_tls),
            HttpConnection::Role::kServer, "", options.connection,
            std::chrono::steady_clock::now() + 2s);
      else
        connected = HttpConnection::Create(
            std::move(socket), HttpConnection::Role::kServer, version);
      EXPECT_TRUE(connected.ok()) << connected.status().message;
      if (!connected.ok()) co_return;
      asio::co_spawn(loop,
                     ServeHttpConnection(*connected.value(),
                                         [&server](auto stream) {
                                           return server.HandleHttpStream(
                                               std::move(stream));
                                         }),
                     [&](auto exception, Status status) {
                       EXPECT_FALSE(exception);
                       EXPECT_TRUE(status.ok()) << status.message;
                       ++joined;
                     });
    }
  };
  asio::steady_timer watchdog(loop);
  watchdog.expires_after(5s);
  watchdog.async_wait([&](asio::error_code error) {
    if (!error) {
      ADD_FAILURE() << "managed connection stalled";
      loop.stop();
    }
  });
  asio::co_spawn(loop, accept(), [&](auto exception) {
    EXPECT_FALSE(exception);
    accept_joined = true;
  });
  auto invoke = [&]() -> asio::awaitable<void> {
    Message message;
    message.set_value("pooled request");
    for (int i = 0; i < 3; ++i) {
      auto response =
          co_await client.Unary<Message, Message>("/test/Say", message);
      EXPECT_TRUE(response.status.ok()) << response.status.message;
      if (response.message) {
        EXPECT_EQ(response.message->value(), message.value());
        ++completed;
      }
      if (i == 0) {
        for (Protocol protocol :
             {Protocol::kConnect, Protocol::kGrpc, Protocol::kGrpcWeb}) {
          if (version == HttpVersion::kHttp1 && protocol == Protocol::kGrpc)
            continue;
          for (int shape = 0; shape < (version == HttpVersion::kHttp2 ? 3 : 2);
               ++shape) {
            StreamCallOptions stream_options;
            stream_options.protocol = protocol;
            Result<TypedStream<Message, Message>> opened;
            if (shape == 0)
              opened = co_await client.ServerStream<Message, Message>(
                  "/test/Expand", message, stream_options);
            else if (shape == 1)
              opened = co_await client.ClientStream<Message, Message>(
                  "/test/Collect", stream_options);
            else
              opened = co_await client.BidiStream<Message, Message>(
                  "/test/Chat", stream_options);
            EXPECT_TRUE(opened.ok()) << opened.status().message;
            if (!opened.ok()) continue;
            auto stream = *opened.value();
            // Hold a lease while trying another call: h2 multiplexes, h1's
            // single-connection pool returns capacity without opening a socket.
            auto overlap =
                co_await client.Unary<Message, Message>("/test/Say", message);
            EXPECT_EQ(overlap.status.code, version == HttpVersion::kHttp2
                                               ? Code::kOk
                                               : Code::kResourceExhausted);
            if (shape != 0) {
              EXPECT_TRUE((co_await stream.Write(message)).ok());
              EXPECT_TRUE((co_await stream.CloseSend()).ok());
            }
            auto next = co_await stream.Read();
            EXPECT_TRUE(next.ok() && *next.value());
            if (next.ok() && *next.value())
              EXPECT_EQ((**next.value()).value(), message.value());
            auto end = co_await stream.Read();
            EXPECT_TRUE(end.ok() && !*end.value());
            auto result = co_await stream.Finish();
            EXPECT_TRUE(result.status.ok()) << result.status.message;
          }
        }
      }
      EXPECT_EQ(accepted, i < 2 ? 1 : 2);
      if (i == 1) {
        asio::steady_timer pause(loop);
        pause.expires_after(100ms);
        co_await pause.async_wait(asio::as_tuple(asio::use_awaitable));
        EXPECT_EQ(joined, 1);
      }
    }
    auto closed =
        co_await transport->Close(std::chrono::steady_clock::now() + 1s);
    EXPECT_TRUE(closed.ok()) << closed.message;
    auto denied = co_await client.Unary<Message, Message>("/test/Say", message);
    EXPECT_EQ(denied.status.code, Code::kUnavailable);
    asio::error_code ignored;
    listener.close(ignored);
    watchdog.cancel();
  };
  asio::co_spawn(loop, invoke(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  loop.run();
  EXPECT_EQ(completed, 3);
  EXPECT_EQ(joined, 2);
  EXPECT_TRUE(accept_joined);
}
INSTANTIATE_TEST_SUITE_P(HttpAndTls, ManagedConnectionTest,
                         testing::Combine(testing::Values(HttpVersion::kHttp1,
                                                          HttpVersion::kHttp2),
                                          testing::Bool()));
TEST(ManagedTransport, DialDeadlineAndCloseDuringTlsHandshake) {
  for (bool close_during_dial : {false, true}) {
    asio::io_context loop;
    asio::ip::tcp::acceptor listener(loop,
                                     {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket peer(loop);
    ManagedTransportOptions options;
    options.connect_timeout = 40ms;
    auto created = ManagedTransport::Create(
        loop.get_executor(),
        "https://localhost:" + std::to_string(listener.local_endpoint().port()),
        options);
    ASSERT_TRUE(created.ok());
    auto transport = *created.value();
    bool finished = false, closed = false;
    auto accept = [&]() -> asio::awaitable<void> {
      auto [error] = co_await listener.async_accept(
          peer, asio::as_tuple(asio::use_awaitable));
      EXPECT_FALSE(error);
      if (close_during_dial) {
        EXPECT_TRUE(
            (co_await transport->Close(std::chrono::steady_clock::now() + 1s))
                .ok());
        closed = true;
      }
    };
    auto invoke = [&]() -> asio::awaitable<void> {
      Metadata headers{{":method", "POST"}, {":path", "/test/Say"}};
      auto stream = co_await transport->Open(std::move(headers));
      EXPECT_EQ(stream.status().code,
                close_during_dial ? Code::kCanceled : Code::kDeadlineExceeded)
          << stream.status().message;
      finished = true;
      if (!close_during_dial) {
        EXPECT_TRUE(
            (co_await transport->Close(std::chrono::steady_clock::now() + 1s))
                .ok());
        closed = true;
      }
    };
    asio::co_spawn(loop, accept(),
                   [](auto exception) { EXPECT_FALSE(exception); });
    asio::co_spawn(loop, invoke(),
                   [](auto exception) { EXPECT_FALSE(exception); });
    loop.run();
    EXPECT_TRUE(finished);
    EXPECT_TRUE(closed);
  }
}
}  // namespace
}  // namespace connectrpc
