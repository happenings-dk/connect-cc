#include "connectrpc/http_connection.h"

#include <openssl/ssl.h>

#include <array>
#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "connectrpc/rpc.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
using Connected = Result<std::shared_ptr<HttpConnection>>;
using Role = HttpConnection::Role;
using TlsSocket = asio::ssl::stream<asio::ip::tcp::socket>;
struct SocketPair {
  asio::ip::tcp::socket client;
  asio::ip::tcp::socket server;
};
class NegotiatedConnectionTest : public testing::Test {
 protected:
  void Configure(TlsAlpn server_alpn, bool mutual = false) {
    TlsOptions options;
    options.certificate_file = CONNECTCPP_TEST_CERT;
    options.private_key_file = CONNECTCPP_TEST_KEY;
    options.trust_file = CONNECTCPP_TEST_CERT;
    options.alpn = server_alpn;
    options.require_client_certificate = mutual;
    ASSERT_TRUE(ConfigureTlsContext(server_tls_, true, options).ok());
    if (!mutual) {
      options.certificate_file.clear();
      options.private_key_file.clear();
    }
    options.require_client_certificate = false;
    ASSERT_TRUE(ConfigureTlsContext(client_tls_, false, options).ok());
  }
  SocketPair Pair() {
    asio::ip::tcp::acceptor acceptor(loop_,
                                     {asio::ip::address_v4::loopback(), 0});
    SocketPair pair{asio::ip::tcp::socket(loop_), asio::ip::tcp::socket(loop_)};
    asio::error_code error;
    pair.client.connect(acceptor.local_endpoint(), error);
    EXPECT_FALSE(error);
    acceptor.accept(pair.server, error);
    EXPECT_FALSE(error);
    return pair;
  }
  void Watch(int tasks) {
    done_ = 0;
    tasks_ = tasks;
    loop_.restart();
    watchdog_.expires_after(std::chrono::seconds(3));
    watchdog_.async_wait([this](asio::error_code error) {
      if (!error) {
        ADD_FAILURE() << "negotiated connection stalled";
        loop_.stop();
      }
    });
  }
  void Done() {
    if (++done_ == tasks_) watchdog_.cancel();
  }
  void Run() {
    loop_.run();
    EXPECT_EQ(done_, tasks_);
  }
  StreamExchange Exchange(std::shared_ptr<HttpConnection> connection) {
    return [connection](Metadata headers, TransportOptions options) {
      headers.emplace(headers.begin(), ":authority", "localhost");
      headers.emplace(headers.begin(), ":scheme", "https");
      return connection->Open(std::move(headers), options.deadline);
    };
  }
  asio::io_context loop_;
  asio::ssl::context client_tls_{asio::ssl::context::tls_client};
  asio::ssl::context server_tls_{asio::ssl::context::tls_server};
  asio::steady_timer watchdog_{loop_};
  int done_ = 0;
  int tasks_ = 0;
};
TEST_F(NegotiatedConnectionTest, PolicyMatrixAndRpcHandoff) {
  using Message = google::protobuf::StringValue;
  for (TlsAlpn server_policy :
       {TlsAlpn::kHttp1, TlsAlpn::kHttp2, TlsAlpn::kHttp2OrHttp1}) {
    for (TlsAlpn client_policy :
         {TlsAlpn::kHttp1, TlsAlpn::kHttp2, TlsAlpn::kHttp2OrHttp1}) {
      for (bool advertise : {false, true}) {
        SCOPED_TRACE(static_cast<int>(server_policy));
        SCOPED_TRACE(static_cast<int>(client_policy));
        SCOPED_TRACE(advertise);
        // Every successful negotiated path also exercises mutual TLS.
        Configure(server_policy, true);
        if (!advertise)
          SSL_CTX_set_alpn_select_cb(server_tls_.native_handle(), nullptr,
                                     nullptr);
        auto pair = Pair();
        bool overlap = server_policy == client_policy ||
                       server_policy == TlsAlpn::kHttp2OrHttp1 ||
                       client_policy == TlsAlpn::kHttp2OrHttp1;
        bool client_ok = advertise ? overlap : client_policy != TlsAlpn::kHttp2;
        bool server_ok = advertise ? overlap : server_policy != TlsAlpn::kHttp2;
        bool serve = client_ok && server_ok;
        HttpVersion expected = advertise && server_policy != TlsAlpn::kHttp1 &&
                                       client_policy != TlsAlpn::kHttp1
                                   ? HttpVersion::kHttp2
                                   : HttpVersion::kHttp1;
        Watch(serve ? 3 : 2);
        Server router;
        int calls = 0;
        ASSERT_TRUE((router.RegisterUnary<Message, Message>(
                         "/test.Service/Echo",
                         [&](Request<Message> request)
                             -> asio::awaitable<Response<Message>> {
                           ++calls;
                           co_return Response<Message>{{},
                                                       request.message,
                                                       {{"reply", "yes"}},
                                                       {{"done", "yes"}}};
                         }))
                        .ok());
        ASSERT_TRUE(
            (router.RegisterBidiStream<Message, Message>(
                 "/test.Service/Chat",
                 [](CallContext, TypedStream<Message, Message> stream)
                     -> asio::awaitable<StreamCompletion> {
                   auto message = co_await stream.Read();
                   EXPECT_TRUE(message.ok());
                   if (message.ok() && *message.value())
                     EXPECT_TRUE(
                         (co_await stream.Write(**message.value())).ok());
                   auto end = co_await stream.Read();
                   EXPECT_TRUE(end.ok() && !*end.value());
                   co_return StreamCompletion{};
                 }))
                .ok());
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto server_operation = [&]() -> asio::awaitable<void> {
          HttpConnectionOptions options;
          options.alpn = server_policy;
          auto connection = co_await HttpConnection::CreateTls(
              TlsSocket(std::move(pair.server), server_tls_), Role::kServer, "",
              options, deadline);
          EXPECT_EQ(connection.ok(), server_ok) << connection.status().message;
          if (!connection.ok()) co_return;
          EXPECT_EQ((*connection.value())->http_version(), expected);
          if (!serve) co_return;
          co_await ServeHttpConnection(
              *connection.value(), [&](std::shared_ptr<ByteStream> stream) {
                EXPECT_EQ(stream->http_version(), expected);
                return router.HandleHttpStream(std::move(stream));
              });
        };
        auto client_operation = [&]() -> asio::awaitable<void> {
          HttpConnectionOptions options;
          options.alpn = client_policy;
          auto connected = co_await HttpConnection::CreateTls(
              TlsSocket(std::move(pair.client), client_tls_), Role::kClient,
              "localhost", options, deadline);
          EXPECT_EQ(connected.ok(), client_ok) << connected.status().message;
          if (!connected.ok()) co_return;
          auto connection = *connected.value();
          EXPECT_EQ(connection->http_version(), expected);
          if (!serve) co_return;
          asio::co_spawn(loop_, connection->Run(), [&](auto exception, Status) {
            EXPECT_FALSE(exception);
            Done();
          });
          Client client({}, Exchange(connection));
          Message input;
          input.set_value("negotiated TLS");
          for (Protocol protocol :
               {Protocol::kConnect, Protocol::kGrpcWeb, Protocol::kGrpc}) {
            CallOptions call;
            call.protocol = protocol;
            auto response = co_await client.Unary<Message, Message>(
                "/test.Service/Echo", input, call);
            if (expected == HttpVersion::kHttp1 &&
                protocol == Protocol::kGrpc) {
              EXPECT_FALSE(response.status.ok());
            } else {
              EXPECT_TRUE(response.status.ok()) << response.status.message;
              if (response.message)
                EXPECT_EQ(response.message->value(), input.value());
              EXPECT_EQ(response.headers, (Metadata{{"reply", "yes"}}));
              EXPECT_EQ(response.trailers, (Metadata{{"done", "yes"}}));
            }
          }
          auto opened = co_await client.BidiStream<Message, Message>(
              "/test.Service/Chat");
          if (expected == HttpVersion::kHttp1) {
            EXPECT_EQ(opened.status().code, Code::kUnimplemented);
          } else {
            EXPECT_TRUE(opened.ok()) << opened.status().message;
            if (opened.ok()) {
              auto stream = std::move(*opened.value());
              EXPECT_TRUE((co_await stream.Write(input)).ok());
              EXPECT_TRUE((co_await stream.CloseSend()).ok());
              auto response = co_await stream.Read();
              EXPECT_TRUE(response.ok() && *response.value());
              auto end = co_await stream.Read();
              EXPECT_TRUE(end.ok() && !*end.value());
              EXPECT_TRUE((co_await stream.Finish()).status.ok());
            }
          }
          co_await connection->Shutdown(std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(100));
        };
        auto finished = [&](auto exception) {
          EXPECT_FALSE(exception);
          Done();
        };
        asio::co_spawn(loop_, server_operation(), finished);
        asio::co_spawn(loop_, client_operation(), finished);
        Run();
        EXPECT_EQ(calls, serve ? (expected == HttpVersion::kHttp1 ? 2 : 3) : 0);
      }
    }
  }
}
TEST_F(NegotiatedConnectionTest, StalledHandshakeDeadlineAndCancellation) {
  Configure(TlsAlpn::kHttp2OrHttp1);
  for (Role role : {Role::kClient, Role::kServer}) {
    for (int mode = 0; mode < 4; ++mode) {
      SCOPED_TRACE(static_cast<int>(role));
      SCOPED_TRACE(mode);
      Watch(2);
      auto pair = Pair();
      auto socket = role == Role::kClient ? std::move(pair.client)
                                          : std::move(pair.server);
      auto peer = role == Role::kClient ? std::move(pair.server)
                                        : std::move(pair.client);
      auto deadline = std::chrono::steady_clock::now() +
                      (mode == 0   ? std::chrono::milliseconds(-1)
                       : mode == 1 ? std::chrono::milliseconds(20)
                                   : std::chrono::seconds(2));
      asio::cancellation_signal signal;
      asio::steady_timer cancel(loop_);
      if (mode == 3) {
        cancel.expires_after(std::chrono::milliseconds(10));
        cancel.async_wait([&](asio::error_code error) {
          if (!error) signal.emit(asio::cancellation_type::all);
        });
      }
      auto read_peer = [&]() -> asio::awaitable<void> {
        std::array<char, 4096> buffer;
        for (;;) {
          auto [error, size] = co_await peer.async_read_some(
              asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
          if (error) break;
          EXPECT_GT(size, 0);
        }
      };
      asio::co_spawn(loop_, read_peer(), [&](auto exception) {
        EXPECT_FALSE(exception);
        Done();
      });
      auto negotiate = [&]() -> asio::awaitable<Connected> {
        // Let the caller enter before canceling: co_spawn itself throws when
        // canceled before its first dispatch, outside the library operation.
        co_await asio::this_coro::throw_if_cancelled(false);
        if (mode == 2) signal.emit(asio::cancellation_type::all);
        co_return co_await HttpConnection::CreateTls(
            TlsSocket(std::move(socket),
                      role == Role::kClient ? client_tls_ : server_tls_),
            role, "localhost", {}, deadline);
      };
      asio::co_spawn(
          loop_, negotiate(),
          asio::bind_cancellation_slot(
              signal.slot(), [&](auto exception, Connected result) {
                EXPECT_FALSE(exception);
                EXPECT_EQ(result.status().code,
                          mode < 2 ? Code::kDeadlineExceeded : Code::kCanceled);
                Done();
              }));
      Run();
    }
  }
}
TEST_F(NegotiatedConnectionTest, VerifiesServerIdentityAndTrust) {
  for (bool trusted : {false, true}) {
    Configure(TlsAlpn::kHttp2OrHttp1);
    if (!trusted)
      SSL_CTX_set_cert_store(client_tls_.native_handle(), X509_STORE_new());
    auto pair = Pair();
    Watch(2);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    asio::co_spawn(loop_,
                   HttpConnection::CreateTls(
                       TlsSocket(std::move(pair.server), server_tls_),
                       Role::kServer, "", {}, deadline),
                   [&](auto exception, Connected result) {
                     EXPECT_FALSE(exception);
                     EXPECT_FALSE(result.ok());
                     Done();
                   });
    asio::co_spawn(
        loop_,
        HttpConnection::CreateTls(
            TlsSocket(std::move(pair.client), client_tls_), Role::kClient,
            trusted ? "wrong.example" : "localhost", {}, deadline),
        [&](auto exception, Connected result) {
          EXPECT_FALSE(exception);
          EXPECT_EQ(result.status().code, Code::kUnavailable);
          Done();
        });
    Run();
  }
}
TEST_F(NegotiatedConnectionTest, CleartextVersionsAndOperationLifetime) {
  for (HttpVersion version : {HttpVersion::kHttp1, HttpVersion::kHttp2}) {
    Watch(4);
    auto pair = Pair();
    auto client_result =
        HttpConnection::Create(std::move(pair.client), Role::kClient, version);
    auto server_result =
        HttpConnection::Create(std::move(pair.server), Role::kServer, version);
    ASSERT_TRUE(client_result.ok());
    ASSERT_TRUE(server_result.ok());
    auto client = std::move(*client_result.value());
    auto server = std::move(*server_result.value());
    asio::co_spawn(loop_, client->Run(), [&](auto exception, Status) {
      EXPECT_FALSE(exception);
      Done();
    });
    asio::co_spawn(loop_, server->Run(), [&](auto exception, Status) {
      EXPECT_FALSE(exception);
      Done();
    });
    auto accepted = server->Accept();
    auto opened = client->Open({{":method", "POST"},
                                {":scheme", "http"},
                                {":authority", "localhost"},
                                {":path", "/echo"}});
    auto cancel_server = server->Cancel();
    auto cancel_client = client->Cancel();
    server.reset();
    client.reset();
    auto serve = [&]() -> asio::awaitable<void> {
      auto result = co_await std::move(accepted);
      EXPECT_TRUE(result.ok());
      if (!result.ok()) co_return;
      EXPECT_EQ((*result.value())->http_version(), version);
      EXPECT_TRUE(
          (co_await (*result.value())->SendHeaders({{":status", "200"}})).ok());
      EXPECT_TRUE((co_await (*result.value())->CloseSend({})).ok());
      EXPECT_TRUE((co_await (*result.value())->FinishSend()).ok());
    };
    auto invoke = [&]() -> asio::awaitable<void> {
      auto result = co_await std::move(opened);
      EXPECT_TRUE(result.ok());
      if (!result.ok()) co_return;
      auto stream = *result.value();
      EXPECT_TRUE((co_await stream->CloseSend({})).ok());
      EXPECT_TRUE((co_await stream->ReadHeaders()).ok());
      auto end = co_await stream->Read();
      EXPECT_TRUE(end.ok() && !*end.value());
      EXPECT_TRUE((co_await stream->Finish()).ok());
      co_await std::move(cancel_client);
      co_await std::move(cancel_server);
    };
    auto finished = [&](auto exception) {
      EXPECT_FALSE(exception);
      Done();
    };
    asio::co_spawn(loop_, serve(), finished);
    asio::co_spawn(loop_, invoke(), finished);
    Run();
  }
}
TEST_F(NegotiatedConnectionTest, HandshakeDeadlineDoesNotLimitRpcLifetime) {
  using Message = google::protobuf::StringValue;
  Configure(TlsAlpn::kHttp2OrHttp1);
  Watch(3);
  auto pair = Pair();
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  Server router;
  ASSERT_TRUE(
      (router.RegisterUnary<Message, Message>(
           "/test.Service/Echo",
           [](Request<Message> request) -> asio::awaitable<Response<Message>> {
             co_return Response<Message>{
                 {}, std::move(request.message), {}, {}};
           }))
          .ok());
  auto serve = [&]() -> asio::awaitable<void> {
    auto connected = co_await HttpConnection::CreateTls(
        TlsSocket(std::move(pair.server), server_tls_), Role::kServer, "", {},
        deadline);
    EXPECT_TRUE(connected.ok()) << connected.status().message;
    if (!connected.ok()) co_return;
    co_await ServeHttpConnection(
        *connected.value(), [&](std::shared_ptr<ByteStream> stream) {
          return router.HandleHttpStream(std::move(stream));
        });
  };
  auto invoke = [&]() -> asio::awaitable<void> {
    auto connected = co_await HttpConnection::CreateTls(
        TlsSocket(std::move(pair.client), client_tls_), Role::kClient,
        "localhost", {}, deadline);
    EXPECT_TRUE(connected.ok()) << connected.status().message;
    if (!connected.ok()) {
      Done();
      co_return;
    }
    auto connection = *connected.value();
    asio::co_spawn(loop_, connection->Run(), [&](auto exception, Status) {
      EXPECT_FALSE(exception);
      Done();
    });
    asio::steady_timer delay(loop_);
    delay.expires_at(deadline + std::chrono::milliseconds(20));
    auto [error] =
        co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(error);
    Client client({}, Exchange(connection));
    auto response = co_await client.Unary<Message, Message>(
        "/test.Service/Echo", Message{});
    EXPECT_TRUE(response.status.ok()) << response.status.message;
    co_await connection->Cancel();
  };
  auto finished = [&](auto exception) {
    EXPECT_FALSE(exception);
    Done();
  };
  asio::co_spawn(loop_, serve(), finished);
  asio::co_spawn(loop_, invoke(), finished);
  Run();
}
TEST_F(NegotiatedConnectionTest, RejectsInvalidSetupAndSecondHandshake) {
  Configure(TlsAlpn::kHttp2OrHttp1);
  for (bool http1 : {false, true}) {
    TlsSocket socket(loop_, client_tls_);
    SSL_set_connect_state(socket.native_handle());
    EXPECT_LE(SSL_do_handshake(socket.native_handle()), 0);
    ASSERT_FALSE(SSL_in_before(socket.native_handle()));
    Status status =
        http1 ? Http1Connection::CreateTls(std::move(socket),
                                           Http1Role::kClient, "localhost")
                    .status()
              : Http2Connection::CreateTls(
                    std::move(socket), Http2Session::Role::kClient, "localhost")
                    .status();
    EXPECT_EQ(status.code, Code::kFailedPrecondition);
  }
  EXPECT_FALSE(HttpConnection::Create(asio::ip::tcp::socket(loop_),
                                      static_cast<Role>(99),
                                      HttpVersion::kHttp1)
                   .ok());
  EXPECT_FALSE(HttpConnection::Create(asio::ip::tcp::socket(loop_),
                                      Role::kClient,
                                      static_cast<HttpVersion>(99))
                   .ok());
  for (int mode = 0; mode < 4; ++mode) {
    Watch(1);
    HttpConnectionOptions options;
    if (mode == 1) options.alpn = static_cast<TlsAlpn>(99);
    TlsSocket socket(loop_, client_tls_);
    if (mode == 3) {
      SSL_set_connect_state(socket.native_handle());
      EXPECT_LE(SSL_do_handshake(socket.native_handle()), 0);
      ASSERT_FALSE(SSL_in_before(socket.native_handle()));
    }
    asio::co_spawn(loop_,
                   HttpConnection::CreateTls(
                       std::move(socket),
                       mode == 0 ? static_cast<Role>(99) : Role::kClient,
                       mode == 2 ? "" : "localhost", options),
                   [&](auto exception, Connected result) {
                     EXPECT_FALSE(exception);
                     EXPECT_EQ(result.status().code,
                               mode == 3 ? Code::kFailedPrecondition
                                         : Code::kInvalidArgument);
                     Done();
                   });
    Run();
  }
  Watch(2);
  auto pair = Pair();
  TlsSocket client(std::move(pair.client), client_tls_);
  TlsSocket server(std::move(pair.server), server_tls_);
  ASSERT_TRUE(
      ConfigureTlsClient(client, "localhost", TlsAlpn::kHttp2OrHttp1).ok());
  auto invoke = [&]() -> asio::awaitable<void> {
    auto [error] = co_await client.async_handshake(
        asio::ssl::stream_base::client, asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(error);
    auto result = co_await HttpConnection::CreateTls(
        std::move(client), Role::kClient, "localhost");
    EXPECT_EQ(result.status().code, Code::kFailedPrecondition);
  };
  auto serve = [&]() -> asio::awaitable<void> {
    auto [error] = co_await server.async_handshake(
        asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(error);
    auto result = co_await HttpConnection::CreateTls(std::move(server),
                                                     Role::kServer, "");
    EXPECT_EQ(result.status().code, Code::kFailedPrecondition);
  };
  auto finished = [&](auto exception) {
    EXPECT_FALSE(exception);
    Done();
  };
  asio::co_spawn(loop_, serve(), finished);
  asio::co_spawn(loop_, invoke(), finished);
  Run();
}
}  // namespace
}  // namespace connectrpc
