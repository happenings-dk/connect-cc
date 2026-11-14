#include <openssl/ssl.h>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>

#include "connectrpc/http_connection.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
TEST(TlsShutdown, ExchangesAlertsAndBoundsUncooperativePeers) {
  using namespace std::chrono_literals;
  for (auto version : {HttpVersion::kHttp1, HttpVersion::kHttp2}) {
    for (bool cooperate : {false, true}) {
      asio::io_context loop;
      asio::ssl::context server_context(asio::ssl::context::tls_server),
          client_context(asio::ssl::context::tls_client);
      TlsOptions identity;
      identity.certificate_file = CONNECTCPP_TEST_CERT;
      identity.private_key_file = CONNECTCPP_TEST_KEY;
      identity.alpn =
          version == HttpVersion::kHttp1 ? TlsAlpn::kHttp1 : TlsAlpn::kHttp2;
      ASSERT_TRUE(ConfigureTlsContext(server_context, true, identity).ok());
      TlsOptions trust;
      trust.trust_file = CONNECTCPP_TEST_CERT;
      ASSERT_TRUE(ConfigureTlsContext(client_context, false, trust).ok());
      asio::ip::tcp::acceptor listener(loop,
                                       {asio::ip::address_v4::loopback(), 0});
      asio::ip::tcp::socket client_socket(loop), server_socket(loop);
      client_socket.connect(listener.local_endpoint());
      listener.accept(server_socket);
      asio::ssl::stream<asio::ip::tcp::socket> client(std::move(client_socket),
                                                      client_context);
      ASSERT_TRUE(ConfigureTlsClient(client, "localhost", identity.alpn).ok());
      HttpConnectionOptions options;
      options.alpn = identity.alpn;
      int done = 0;
      bool run_done = false;
      auto serve = [&]() -> asio::awaitable<void> {
        auto connection = co_await HttpConnection::CreateTls(
            asio::ssl::stream<asio::ip::tcp::socket>(std::move(server_socket),
                                                     server_context),
            HttpConnection::Role::kServer, "", options,
            std::chrono::steady_clock::now() + 1s);
        EXPECT_TRUE(connection.ok());
        if (!connection.ok()) co_return;
        asio::co_spawn(loop, (*connection.value())->Run(),
                       [&](auto exception, Status) {
                         EXPECT_FALSE(exception);
                         run_done = true;
                       });
        auto closed = co_await (*connection.value())
                          ->Shutdown(std::chrono::steady_clock::now() + 100ms);
        EXPECT_EQ(closed.code, cooperate ? Code::kOk : Code::kDeadlineExceeded)
            << closed.message;
        ++done;
      };
      auto peer = [&]() -> asio::awaitable<void> {
        auto [error] = co_await client.async_handshake(
            asio::ssl::stream_base::client,
            asio::as_tuple(asio::use_awaitable));
        EXPECT_FALSE(error);
        if (error) co_return;
        // Read the HTTP frames and the peer's alert. A noncooperating peer
        // deliberately withholds its alert even after receiving close_notify.
        char bytes[4096];
        for (;;) {
          auto [read_error, size] = co_await client.async_read_some(
              asio::buffer(bytes), asio::as_tuple(asio::use_awaitable));
          (void)size;
          if (read_error) {
            EXPECT_EQ(read_error, asio::error::eof);
            break;
          }
        }
        EXPECT_NE(
            SSL_get_shutdown(client.native_handle()) & SSL_RECEIVED_SHUTDOWN,
            0);
        if (cooperate) {
          auto [shutdown_error] = co_await client.async_shutdown(
              asio::as_tuple(asio::use_awaitable));
          EXPECT_FALSE(shutdown_error) << shutdown_error.message();
          EXPECT_NE(
              SSL_get_shutdown(client.native_handle()) & SSL_SENT_SHUTDOWN, 0);
        }
        ++done;
      };
      asio::co_spawn(loop, serve(),
                     [](auto exception) { EXPECT_FALSE(exception); });
      asio::co_spawn(loop, peer(),
                     [](auto exception) { EXPECT_FALSE(exception); });
      loop.run();
      EXPECT_EQ(done, 2);
      EXPECT_TRUE(run_done);
    }
  }
}
}  // namespace
}  // namespace connectrpc
