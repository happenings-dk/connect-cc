#include "connectrpc/tls.h"

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "connectrpc/transport.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
TEST(Tls, ExplicitAlpnNegotiationAndFallback) {
  for (TlsAlpn server_policy :
       {TlsAlpn::kHttp2, TlsAlpn::kHttp1, TlsAlpn::kHttp2OrHttp1}) {
    for (TlsAlpn client_policy :
         {TlsAlpn::kHttp2, TlsAlpn::kHttp1, TlsAlpn::kHttp2OrHttp1}) {
      for (bool advertise : {false, true}) {
        SCOPED_TRACE(static_cast<int>(server_policy));
        SCOPED_TRACE(static_cast<int>(client_policy));
        SCOPED_TRACE(advertise);
        asio::io_context loop;
        asio::ssl::context server_context(asio::ssl::context::tls_server);
        asio::ssl::context client_context(asio::ssl::context::tls_client);
        TlsOptions server_options;
        server_options.certificate_file = CONNECTCPP_TEST_CERT;
        server_options.private_key_file = CONNECTCPP_TEST_KEY;
        server_options.trust_file = CONNECTCPP_TEST_CERT;
        server_options.alpn = server_policy;
        ASSERT_TRUE(
            ConfigureTlsContext(server_context, true, server_options).ok());
        if (!advertise)
          SSL_CTX_set_alpn_select_cb(server_context.native_handle(), nullptr,
                                     nullptr);
        TlsOptions client_options;
        client_options.trust_file = CONNECTCPP_TEST_CERT;
        ASSERT_TRUE(
            ConfigureTlsContext(client_context, false, client_options).ok());
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
        asio::ssl::stream<asio::ip::tcp::socket> client(loop, client_context);
        asio::ssl::stream<asio::ip::tcp::socket> server(loop, server_context);
        client.next_layer().connect(endpoint, error);
        ASSERT_FALSE(error);
        acceptor.accept(server.next_layer(), error);
        ASSERT_FALSE(error);
        ASSERT_TRUE(
            ConfigureTlsClient(client, "localhost", client_policy).ok());
        if (client_policy == TlsAlpn::kHttp2OrHttp1) {
          constexpr unsigned char reverse_offers[] = {
              8, 'h', 't', 't', 'p', '/', '1', '.', '1', 2, 'h', '2'};
          ASSERT_EQ(SSL_set_alpn_protos(client.native_handle(), reverse_offers,
                                        sizeof(reverse_offers)),
                    0);
        }
        EXPECT_EQ(NegotiatedTlsProtocol(client, client_policy).status().code,
                  Code::kFailedPrecondition);
        bool overlap = server_policy == client_policy ||
                       server_policy == TlsAlpn::kHttp2OrHttp1 ||
                       client_policy == TlsAlpn::kHttp2OrHttp1;
        int completed = 0;
        asio::steady_timer watchdog(loop);
        watchdog.expires_after(std::chrono::seconds(3));
        watchdog.async_wait([&](asio::error_code timer_error) {
          if (!timer_error) {
            ADD_FAILURE() << "TLS negotiation did not complete";
            asio::error_code ignored;
            client.next_layer().close(ignored);
            server.next_layer().close(ignored);
          }
        });
        auto client_handshake = [&]() -> asio::awaitable<void> {
          auto [handshake_error] = co_await client.async_handshake(
              asio::ssl::stream_base::client,
              asio::as_tuple(asio::use_awaitable));
          EXPECT_EQ(!handshake_error, !advertise || overlap);
          if (!handshake_error) {
            auto protocol = NegotiatedTlsProtocol(client, client_policy);
            if (!advertise && client_policy == TlsAlpn::kHttp2) {
              EXPECT_EQ(protocol.status().code, Code::kUnavailable);
            } else {
              EXPECT_TRUE(protocol.ok()) << protocol.status().message;
              if (!protocol.ok()) co_return;
              auto expected = advertise && server_policy != TlsAlpn::kHttp1 &&
                                      client_policy != TlsAlpn::kHttp1
                                  ? TlsProtocol::kHttp2
                                  : TlsProtocol::kHttp1;
              EXPECT_EQ(*protocol.value(), expected);
              EXPECT_EQ(CheckHttp2Alpn(client).ok(),
                        expected == TlsProtocol::kHttp2);
              auto disallowed = expected == TlsProtocol::kHttp2
                                    ? TlsAlpn::kHttp1
                                    : TlsAlpn::kHttp2;
              EXPECT_FALSE(NegotiatedTlsProtocol(client, disallowed).ok());
            }
          }
        };
        auto server_handshake = [&]() -> asio::awaitable<void> {
          auto [handshake_error] = co_await server.async_handshake(
              asio::ssl::stream_base::server,
              asio::as_tuple(asio::use_awaitable));
          EXPECT_EQ(!handshake_error, !advertise || overlap);
        };
        auto finished = [&](auto exception) {
          EXPECT_FALSE(exception);
          if (++completed == 2) watchdog.cancel();
        };
        asio::co_spawn(loop, client_handshake(), finished);
        asio::co_spawn(loop, server_handshake(), finished);
        loop.run();
        EXPECT_EQ(completed, 2);
      }
    }
  }
}
struct TlsCase {
  std::string hostname;
  bool trust_server;
  bool require_client;
  bool client_identity;
  bool success;
  bool alpn = true;
  bool replace_trust = false;
};
class TlsTest : public testing::TestWithParam<TlsCase> {};
TEST_P(TlsTest, VerifiedUnary) {
  const TlsCase& test = GetParam();
  asio::io_context loop;
  asio::ssl::context server_context(asio::ssl::context::tls_server);
  asio::ssl::context client_context(asio::ssl::context::tls_client);
  TlsOptions server_options;
  server_options.trust_file = CONNECTCPP_TEST_CERT;
  server_options.certificate_file = CONNECTCPP_TEST_CERT;
  server_options.private_key_file = CONNECTCPP_TEST_KEY;
  server_options.require_client_certificate = test.require_client;
  ASSERT_TRUE(ConfigureTlsContext(server_context, true, server_options).ok());
  if (!test.alpn)
    SSL_CTX_set_alpn_select_cb(server_context.native_handle(), nullptr,
                               nullptr);
  TlsOptions client_options;
  if (test.replace_trust) {
    TlsOptions previous;
    previous.trust_file = CONNECTCPP_TEST_CERT;
    ASSERT_TRUE(ConfigureTlsContext(client_context, false, previous).ok());
    client_options.trust = TlsTrust::kOpenSsl;
  }
  if (test.trust_server) client_options.trust_file = CONNECTCPP_TEST_CERT;
  if (test.client_identity) {
    client_options.certificate_file = CONNECTCPP_TEST_CERT;
    client_options.private_key_file = CONNECTCPP_TEST_KEY;
  }
  ASSERT_TRUE(ConfigureTlsContext(client_context, false, client_options).ok());
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
  asio::ip::tcp::socket client(loop);
  client.connect(endpoint, error);
  ASSERT_FALSE(error);
  asio::ip::tcp::socket server(loop);
  acceptor.accept(server, error);
  ASSERT_FALSE(error);
  bool done = false;
  bool handled = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  asio::co_spawn(loop,
                 ServeHttp2Tls(
                     asio::ssl::stream<asio::ip::tcp::socket>(std::move(server),
                                                              server_context),
                     [&handled](HttpMessage request) {
                       handled = true;
                       return HttpMessage{
                           0, {{":status", "200"}}, request.body};
                     },
                     deadline),
                 [](auto exception, Status) { EXPECT_FALSE(exception); });
  TransportOptions options;
  options.deadline = deadline;
  asio::co_spawn(loop,
                 UnaryHttp2Tls(asio::ssl::stream<asio::ip::tcp::socket>(
                                   std::move(client), client_context),
                               test.hostname,
                               {{":method", "POST"},
                                {":scheme", "https"},
                                {":authority", "localhost"},
                                {":path", "/echo"}},
                               "hello TLS", options),
                 [&done, &test](auto exception, Result<HttpMessage> response) {
                   EXPECT_FALSE(exception);
                   EXPECT_EQ(response.ok(), test.success)
                       << response.status().message;
                   if (response.ok())
                     EXPECT_EQ(response.value()->body, "hello TLS");
                   done = true;
                 });
  loop.run();
  EXPECT_TRUE(done);
  EXPECT_EQ(handled, test.success);
}
TEST(Tls, CancellationAcrossHandshakeAndExchange) {
  for (int delay_ms : {0, 1, 2, 3, 5, 10}) {
    SCOPED_TRACE(delay_ms);
    asio::io_context loop;
    asio::ssl::context server_context(asio::ssl::context::tls_server);
    asio::ssl::context client_context(asio::ssl::context::tls_client);
    TlsOptions identity;
    identity.certificate_file = CONNECTCPP_TEST_CERT;
    identity.private_key_file = CONNECTCPP_TEST_KEY;
    ASSERT_TRUE(ConfigureTlsContext(server_context, true, identity).ok());
    TlsOptions trust;
    trust.trust_file = CONNECTCPP_TEST_CERT;
    ASSERT_TRUE(ConfigureTlsContext(client_context, false, trust).ok());
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
    asio::ip::tcp::socket client(loop);
    client.connect(endpoint, error);
    ASSERT_FALSE(error);
    asio::ip::tcp::socket server(loop);
    acceptor.accept(server, error);
    ASSERT_FALSE(error);
    asio::steady_timer watchdog(loop);
    watchdog.expires_after(std::chrono::seconds(1));
    watchdog.async_wait([&loop](asio::error_code error) {
      if (!error) {
        ADD_FAILURE() << "TLS cancellation failed to interrupt the exchange";
        loop.stop();
      }
    });
    int done = 0;
    auto completed = [&] {
      if (++done == 2) watchdog.cancel();
    };
    UnaryHandler handler = [](HttpMessage) -> asio::awaitable<HttpMessage> {
      co_await asio::this_coro::throw_if_cancelled(false);
      asio::steady_timer timer(co_await asio::this_coro::executor);
      timer.expires_after(std::chrono::seconds(2));
      co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
      co_return HttpMessage{0, {{":status", "200"}}, "late response"};
    };
    asio::co_spawn(loop,
                   ServeHttp2Tls(asio::ssl::stream<asio::ip::tcp::socket>(
                                     std::move(server), server_context),
                                 handler,
                                 std::chrono::steady_clock::now() +
                                     std::chrono::seconds(3)),
                   [&](auto exception, Status) {
                     EXPECT_FALSE(exception);
                     completed();
                   });
    asio::cancellation_signal signal;
    asio::steady_timer cancel_timer(loop);
    cancel_timer.expires_after(std::chrono::milliseconds(delay_ms));
    cancel_timer.async_wait([&](asio::error_code error) {
      if (!error) signal.emit(asio::cancellation_type::all);
    });
    asio::co_spawn(
        loop,
        UnaryHttp2Tls(asio::ssl::stream<asio::ip::tcp::socket>(
                          std::move(client), client_context),
                      "localhost",
                      {{":method", "POST"},
                       {":scheme", "https"},
                       {":authority", "localhost"},
                       {":path", "/echo"}},
                      "request"),
        asio::bind_cancellation_slot(
            signal.slot(), [&](auto exception, Result<HttpMessage> response) {
              EXPECT_FALSE(exception);
              EXPECT_EQ(response.status().code, Code::kCanceled);
              completed();
            }));
    loop.run();
    EXPECT_EQ(done, 2);
  }
}
INSTANTIATE_TEST_SUITE_P(
    Identity, TlsTest,
    testing::Values(TlsCase{"localhost", true, false, false, true},
                    TlsCase{"127.0.0.1", true, false, false, true},
                    TlsCase{"wrong.example", true, false, false, false},
                    TlsCase{"localhost", false, false, false, false},
                    TlsCase{"localhost", true, true, false, false},
                    TlsCase{"localhost", true, true, true, true},
                    TlsCase{"localhost", true, false, false, false, false},
                    TlsCase{"localhost", false, false, false, false, true,
                            true}));
}  // namespace
}  // namespace connectrpc
