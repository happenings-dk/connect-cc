#include "connectrpc/http1_connection.h"

#include <algorithm>
#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <string>
#include <vector>

#include "connectrpc/rpc.h"
#include "connectrpc/tls.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
Metadata H1Request() {
  return {{":method", "POST"}, {":authority", "localhost"}, {":path", "/test"}};
}
asio::awaitable<Result<std::string>> H1ReadAll(
    std::shared_ptr<ByteStream> stream) {
  std::string result;
  for (;;) {
    auto next = co_await stream->Read();
    if (!next.ok()) co_return Result<std::string>(next.status());
    if (!*next.value()) co_return Result<std::string>(std::move(result));
    result.append(**next.value());
  }
}
asio::awaitable<Status> H1WriteAll(std::shared_ptr<ByteStream> stream,
                                   std::string bytes) {
  auto status = co_await stream->Write(std::move(bytes));
  if (!status.ok()) co_return status;
  status = co_await stream->CloseSend({});
  if (!status.ok()) co_return status;
  co_return co_await stream->FinishSend();
}
class Http1ConnectionTest : public testing::TestWithParam<bool> {
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
    client.set_option(asio::socket_base::send_buffer_size(4096), error);
    ASSERT_FALSE(error);
    server.set_option(asio::socket_base::send_buffer_size(4096), error);
    ASSERT_FALSE(error);
    Http1ConnectionOptions options;
    options.limits.max_chunk_bytes = 1024;
    options.limits.max_metadata_bytes = 2048;
    options.max_queued_bytes = 4096;
    Result<std::shared_ptr<Http1Connection>> c;
    Result<std::shared_ptr<Http1Connection>> s;
    if (GetParam()) {
      TlsOptions tls;
      tls.alpn = TlsAlpn::kHttp1;
      tls.certificate_file = CONNECTCPP_TEST_CERT;
      tls.private_key_file = CONNECTCPP_TEST_KEY;
      ASSERT_TRUE(ConfigureTlsContext(server_tls_, true, tls).ok());
      tls = {};
      tls.trust_file = CONNECTCPP_TEST_CERT;
      ASSERT_TRUE(ConfigureTlsContext(client_tls_, false, tls).ok());
      auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(3);
      c = Http1Connection::CreateTls(asio::ssl::stream<asio::ip::tcp::socket>(
                                         std::move(client), client_tls_),
                                     Http1Role::kClient, "localhost", options,
                                     deadline);
      s = Http1Connection::CreateTls(asio::ssl::stream<asio::ip::tcp::socket>(
                                         std::move(server), server_tls_),
                                     Http1Role::kServer, "", options, deadline);
    } else {
      c = Http1Connection::Create(std::move(client), Http1Role::kClient,
                                  options);
      s = Http1Connection::Create(std::move(server), Http1Role::kServer,
                                  options);
    }
    ASSERT_TRUE(c.ok());
    ASSERT_TRUE(s.ok());
    client_ = *c.value();
    server_ = *s.value();
    watchdog_.expires_after(std::chrono::seconds(5));
    watchdog_.async_wait([this](asio::error_code error) {
      if (!error) {
        ADD_FAILURE() << "HTTP/1.1 stalled";
        loop_.stop();
      }
    });
    asio::co_spawn(loop_, client_->Run(), [this](auto error, Status status) {
      EXPECT_FALSE(error);
      OnDone(std::move(status));
    });
    if (run_server_)
      asio::co_spawn(loop_, server_->Run(), [this](auto error, Status status) {
        EXPECT_FALSE(error);
        OnDone(std::move(status));
      });
  }
  void OnDone(Status status) {
    outcomes_.push_back(std::move(status));
    if (outcomes_.size() == 2) watchdog_.cancel();
  }
  void Spawn(asio::awaitable<void> operation) {
    asio::co_spawn(loop_, std::move(operation),
                   [](auto error) { EXPECT_FALSE(error); });
  }
  void Run() {
    loop_.run();
    EXPECT_EQ(outcomes_.size(), 2);
  }
  asio::io_context loop_;
  asio::ssl::context client_tls_{asio::ssl::context::tls_client};
  asio::ssl::context server_tls_{asio::ssl::context::tls_server};
  asio::steady_timer watchdog_{loop_};
  std::shared_ptr<Http1Connection> client_;
  std::shared_ptr<Http1Connection> server_;
  std::vector<Status> outcomes_;
  bool run_server_ = true;
};
TEST_P(Http1ConnectionTest,
       CompleteResponsePreservesBodyTrailersAndBackpressure) {
  int completed = 0;
  bool response_queued = false;
  auto serve = [&]() -> asio::awaitable<void> {
    for (size_t size : {size_t{0}, size_t{58}, size_t{256 * 1024}}) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      auto stream = *accepted.value();
      auto request_body = co_await H1ReadAll(stream);
      EXPECT_TRUE(request_body.ok());
      HttpMessage response;
      response.headers = {{":status", "200"}, {"batch", "yes"}};
      response.body = std::string(size, 'r');
      Metadata trailers{{"complete", "yes"}};
      auto operation =
          stream->SendResponse(std::move(response), std::move(trailers));
      auto sent = co_await std::move(operation);
      EXPECT_TRUE(sent.ok()) << sent.message;
      response_queued = true;
      auto flushed = co_await stream->FinishSend();
      EXPECT_TRUE(flushed.ok()) << flushed.message;
      // The final backpressured response must survive connection closure,
      // including its last body chunk and trailers on Winsock.
      if (size == 256 * 1024) {
        auto closed = co_await server_->Shutdown(
            std::chrono::steady_clock::now() + std::chrono::seconds(2));
        EXPECT_TRUE(closed.ok()) << closed.message;
      }
    }
  };
  auto invoke = [&]() -> asio::awaitable<void> {
    for (size_t size : {size_t{0}, size_t{58}, size_t{256 * 1024}}) {
      response_queued = false;
      auto opened = co_await client_->Open(H1Request());
      EXPECT_TRUE(opened.ok());
      if (!opened.ok()) co_return;
      auto stream = *opened.value();
      EXPECT_TRUE((co_await stream->CloseSend({})).ok());
      auto headers = co_await stream->ReadHeaders();
      EXPECT_TRUE(headers.ok());
      if (headers.ok()) {
        EXPECT_NE(std::find(headers.value()->begin(), headers.value()->end(),
                            Metadata::value_type{"batch", "yes"}),
                  headers.value()->end());
      }
      if (size > 8192) {
        asio::steady_timer delay(co_await asio::this_coro::executor);
        delay.expires_after(std::chrono::milliseconds(50));
        co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
        EXPECT_FALSE(response_queued)
            << "response must await receiver capacity";
      }
      auto body = co_await H1ReadAll(stream);
      EXPECT_TRUE(body.ok()) << body.status().message;
      if (body.ok()) EXPECT_EQ(*body.value(), std::string(size, 'r'));
      auto finish = co_await stream->Finish();
      EXPECT_TRUE(finish.ok()) << finish.status().message;
      if (finish.ok())
        EXPECT_EQ(*finish.value(), (Metadata{{"complete", "yes"}}));
      ++completed;
    }
    auto shutdown = co_await client_->Shutdown(
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    EXPECT_TRUE(shutdown.ok()) << shutdown.message;
  };
  asio::co_spawn(loop_, serve(), [](auto error) { EXPECT_FALSE(error); });
  asio::co_spawn(loop_, invoke(), [](auto error) { EXPECT_FALSE(error); });
  Run();
  EXPECT_EQ(completed, 3);
}
TEST_P(Http1ConnectionTest, KeepAliveChunkedFixedLengthAndTrailers) {
  int completed = 0;
  auto serve = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 3; ++i) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      auto stream = *accepted.value();
      auto headers = co_await stream->ReadHeaders();
      EXPECT_TRUE(headers.ok());
      auto body = co_await H1ReadAll(stream);
      EXPECT_TRUE(body.ok()) << body.status().message;
      if (body.ok()) EXPECT_EQ(*body.value(), std::string(24001, 'x'));
      auto trailers = co_await stream->ReadTrailers();
      EXPECT_TRUE(trailers.ok());
      if (trailers.ok())
        EXPECT_EQ(*trailers.value(),
                  (i == 1 ? Metadata{} : Metadata{{"req", "end"}}));
      Metadata response{{":status", "200"}, {"custom", "header"}};
      if (i == 1) response.emplace_back("content-length", "24001");
      EXPECT_TRUE((co_await stream->SendHeaders(std::move(response))).ok());
      EXPECT_TRUE((co_await stream->Write(std::string(24001, 'r'))).ok());
      EXPECT_TRUE((co_await stream->CloseSend(
                       i == 1 ? Metadata{} : Metadata{{"resp", "end"}}))
                      .ok());
      EXPECT_TRUE((co_await stream->FinishSend()).ok());
    }
  };
  auto invoke = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 3; ++i) {
      auto headers = H1Request();
      if (i == 1) headers.emplace_back("content-length", "24001");
      auto opened = co_await client_->Open(std::move(headers));
      EXPECT_TRUE(opened.ok()) << opened.status().message;
      if (!opened.ok()) co_return;
      auto stream = *opened.value();
      auto overlap = co_await client_->Open(H1Request());
      EXPECT_EQ(overlap.status().code, Code::kResourceExhausted);
      EXPECT_TRUE((co_await stream->Write(std::string(24001, 'x'))).ok());
      EXPECT_TRUE((co_await stream->CloseSend(
                       i == 1 ? Metadata{} : Metadata{{"req", "end"}}))
                      .ok());
      EXPECT_TRUE((co_await stream->ReadHeaders()).ok());
      auto response = co_await H1ReadAll(stream);
      EXPECT_TRUE(response.ok()) << response.status().message;
      if (response.ok()) EXPECT_EQ(*response.value(), std::string(24001, 'r'));
      auto finish = co_await stream->Finish();
      EXPECT_TRUE(finish.ok()) << finish.status().message;
      if (finish.ok())
        EXPECT_EQ(*finish.value(),
                  (i == 1 ? Metadata{} : Metadata{{"resp", "end"}}));
      EXPECT_TRUE((co_await stream->Cancel())
                      .ok());  // A completed call preserves reuse.
      ++completed;
    }
    co_await client_->Cancel();
    co_await server_->Cancel();
  };
  Spawn(serve());
  Spawn(invoke());
  Run();
  EXPECT_EQ(completed, 3);
}
TEST_P(Http1ConnectionTest, EarlyResponseInterruptsBackpressuredUpload) {
  bool upload_done = false;
  bool got_response = false;
  auto serve = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    auto stream = *accepted.value();
    asio::steady_timer delay(co_await asio::this_coro::executor);
    delay.expires_after(std::chrono::milliseconds(25));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(upload_done);
    EXPECT_TRUE((co_await stream->SendHeaders(
                     {{":status", "403"}, {"content-length", "6"}}))
                    .ok());
    EXPECT_TRUE((co_await H1WriteAll(stream, "denied")).ok());
    EXPECT_TRUE((co_await stream->DiscardRead()).ok());
  };
  auto invoke = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(H1Request());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto stream = *opened.value();
    asio::co_spawn(loop_, H1WriteAll(stream, std::string(8 * 1024 * 1024, 'x')),
                   [&](auto error, Status status) {
                     EXPECT_FALSE(error);
                     EXPECT_FALSE(status.ok());
                     upload_done = true;
                   });
    auto headers = co_await stream->ReadHeaders();
    EXPECT_TRUE(headers.ok());
    auto response = co_await H1ReadAll(stream);
    EXPECT_TRUE(response.ok()) << response.status().message;
    if (response.ok()) EXPECT_EQ(*response.value(), "denied");
    got_response = true;
    EXPECT_TRUE((co_await stream->ReadTrailers()).ok());
    co_await stream->Cancel();
  };
  Spawn(serve());
  Spawn(invoke());
  Run();
  EXPECT_TRUE(got_response);
  EXPECT_TRUE(upload_done);
}
TEST_P(Http1ConnectionTest, DeadlineAndBoundedShutdown) {
  bool timed_out = false;
  auto serve = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    // Keep the request unfinished, so EOF is observed as connection loss.
    auto status = co_await (*accepted.value())->WaitClosed();
    EXPECT_FALSE(status.ok());
  };
  auto invoke = [&]() -> asio::awaitable<void> {
    auto opened =
        co_await client_->Open(H1Request(), std::chrono::steady_clock::now() +
                                                std::chrono::milliseconds(80));
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto response = co_await (*opened.value())->ReadHeaders();
    EXPECT_EQ(response.status().code, Code::kDeadlineExceeded);
    timed_out = true;
  };
  Spawn(serve());
  Spawn(invoke());
  Run();
  EXPECT_TRUE(timed_out);
}
TEST_P(Http1ConnectionTest, ServerReadCancellationPreservesResponse) {
  asio::cancellation_signal stop_read;
  bool read_done = false;
  auto serve = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    auto stream = *accepted.value();
    asio::co_spawn(loop_, stream->Read(),
                   asio::bind_cancellation_slot(
                       stop_read.slot(), [&](auto error, auto result) {
                         EXPECT_FALSE(error);
                         EXPECT_EQ(result.status().code, Code::kCanceled);
                         read_done = true;
                       }));
    asio::steady_timer delay(co_await asio::this_coro::executor);
    delay.expires_after(std::chrono::milliseconds(20));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    stop_read.emit(asio::cancellation_type::all);
    EXPECT_TRUE((co_await stream->SendHeaders({{":status", "200"}})).ok());
    EXPECT_TRUE((co_await H1WriteAll(stream, "still writable")).ok());
    co_await stream->DiscardRead();
  };
  auto invoke = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(H1Request());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto response = co_await H1ReadAll(*opened.value());
    EXPECT_TRUE(response.ok());
    if (response.ok()) EXPECT_EQ(*response.value(), "still writable");
    co_await (*opened.value())->Cancel();
  };
  Spawn(serve());
  Spawn(invoke());
  Run();
  EXPECT_TRUE(read_done);
}
TEST_P(Http1ConnectionTest, ShutdownDeadlineAbortsUnfinishedCall) {
  bool shutdown_done = false;
  auto serve = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    auto status = co_await server_->Shutdown(std::chrono::steady_clock::now() +
                                             std::chrono::milliseconds(40));
    EXPECT_EQ(status.code, Code::kDeadlineExceeded);
    shutdown_done = true;
  };
  auto invoke = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(H1Request());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    EXPECT_FALSE((co_await (*opened.value())->ReadHeaders()).ok());
  };
  Spawn(serve());
  Spawn(invoke());
  Run();
  EXPECT_TRUE(shutdown_done);
}
TEST_P(Http1ConnectionTest, CancelSecondCallAndObservePeerClose) {
  asio::cancellation_signal stop_read;
  bool read_done = false;
  bool peer_closed = false;
  auto serve = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 2; ++i) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      auto stream = *accepted.value();
      EXPECT_TRUE((co_await H1ReadAll(stream)).ok());
      if (i == 0) {
        EXPECT_TRUE((co_await stream->SendHeaders({{":status", "200"}})).ok());
        EXPECT_TRUE((co_await H1WriteAll(stream, "first")).ok());
      } else {
        auto status = co_await stream->WaitClosed();
        EXPECT_FALSE(status.ok());
        peer_closed = true;
      }
    }
  };
  auto invoke = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 2; ++i) {
      auto opened = co_await client_->Open(H1Request());
      EXPECT_TRUE(opened.ok());
      if (!opened.ok()) co_return;
      auto stream = *opened.value();
      EXPECT_TRUE((co_await stream->CloseSend()).ok());
      if (i == 0) {
        EXPECT_TRUE((co_await H1ReadAll(stream)).ok());
        EXPECT_TRUE((co_await stream->Finish()).ok());
      } else {
        asio::co_spawn(
            loop_, stream->ReadHeaders(),
            asio::bind_cancellation_slot(
                stop_read.slot(), [&, stream](auto exception, auto result) {
                  EXPECT_FALSE(exception);
                  EXPECT_EQ(result.status().code, Code::kCanceled);
                  read_done = true;
                }));
        asio::steady_timer delay(co_await asio::this_coro::executor);
        delay.expires_after(std::chrono::milliseconds(30));
        co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
        stop_read.emit(asio::cancellation_type::all);
      }
    }
  };
  Spawn(serve());
  Spawn(invoke());
  Run();
  EXPECT_TRUE(read_done);
  EXPECT_TRUE(peer_closed);
}
TEST(Http1Network, InformationalCloseDelimitedAndTruncatedResponses) {
  const std::vector<std::pair<std::string, Code>> cases{
      {"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nlink: "
       "x\r\n\r\nHTTP/1.1 200 OK\r\ncontent-length: 5\r\nconnection: "
       "close\r\n\r\nhello",
       Code::kOk},
      {"HTTP/1.1 200 OK\r\nconnection: close\r\n\r\nhello", Code::kOk},
      {"HTTP/1.1 200 OK\r\ncontent-length: 10\r\n\r\nhello", Code::kDataLoss},
      {"HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhello\r\n",
       Code::kDataLoss}};
  for (const auto& [response, expected] : cases) {
    SCOPED_TRACE(response);
    asio::io_context loop;
    asio::ip::tcp::acceptor listener(loop,
                                     {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket outgoing(loop);
    outgoing.connect(listener.local_endpoint());
    asio::ip::tcp::socket peer(loop);
    listener.accept(peer);
    auto created =
        Http1Connection::Create(std::move(outgoing), Http1Role::kClient);
    ASSERT_TRUE(created.ok());
    auto connection = *created.value();
    asio::steady_timer timeout(loop);
    timeout.expires_after(std::chrono::seconds(3));
    timeout.async_wait([&](asio::error_code error) {
      if (!error) {
        ADD_FAILURE() << "raw HTTP peer stalled";
        loop.stop();
      }
    });
    bool run_done = false;
    bool read_done = false;
    asio::co_spawn(loop, connection->Run(), [&](auto exception, Status) {
      EXPECT_FALSE(exception);
      run_done = true;
      timeout.cancel();
    });
    auto serve = [&]() -> asio::awaitable<void> {
      // Read the empty request before responding, then fragment every byte.
      std::string request;
      while (request.find("\r\n\r\n") == std::string::npos) {
        char buffer[1024];
        auto [error, count] = co_await peer.async_read_some(
            asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
        EXPECT_FALSE(error);
        if (error) co_return;
        request.append(buffer, count);
      }
      for (char byte : response) {
        auto [error, count] = co_await asio::async_write(
            peer, asio::buffer(&byte, 1), asio::as_tuple(asio::use_awaitable));
        EXPECT_FALSE(error);
        EXPECT_EQ(count, 1);
        if (error) co_return;
      }
      asio::error_code ignored;
      peer.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    };
    auto invoke = [&]() -> asio::awaitable<void> {
      auto headers = H1Request();
      headers.emplace_back("content-length", "0");
      auto opened = co_await connection->Open(std::move(headers));
      EXPECT_TRUE(opened.ok());
      if (!opened.ok()) co_return;
      auto stream = *opened.value();
      EXPECT_TRUE((co_await stream->CloseSend()).ok());
      auto received = co_await stream->ReadHeaders();
      EXPECT_TRUE(received.ok());
      if (received.ok()) {
        bool final = false;
        for (const auto& [name, value] : *received.value())
          if (name == ":status") {
            EXPECT_EQ(value, "200");
            final = true;
          }
        EXPECT_TRUE(final);
      }
      auto body = co_await H1ReadAll(stream);
      EXPECT_EQ(body.status().code, expected) << body.status().message;
      if (body.ok()) EXPECT_EQ(*body.value(), "hello");
      read_done = true;
      co_await connection->Cancel();
    };
    asio::co_spawn(loop, serve(),
                   [](auto exception) { EXPECT_FALSE(exception); });
    asio::co_spawn(loop, invoke(),
                   [](auto exception) { EXPECT_FALSE(exception); });
    loop.run();
    EXPECT_TRUE(run_done);
    EXPECT_TRUE(read_done);
  }
}
class Http1ServingTest : public Http1ConnectionTest {
 protected:
  void SetUp() override {
    run_server_ = false;
    Http1ConnectionTest::SetUp();
  }
};
TEST_P(Http1ServingTest, RpcUnaryAndStreamingRoundTrip) {
  using Message = google::protobuf::StringValue;
  using Stream = TypedStream<Message, Message>;
  Server router;
  int calls = 0;
  ASSERT_TRUE(
      (router.RegisterUnary<Message, Message>(
           "/test.Service/Echo",
           [&](Request<Message> request) -> asio::awaitable<Response<Message>> {
             ++calls;
             co_return Response<Message>{{},
                                         std::move(request.message),
                                         {{"reply", "yes"}},
                                         {{"done", "yes"}}};
           },
           {Idempotency::kNoSideEffects}))
          .ok());
  ASSERT_TRUE(
      (router.RegisterServerStream<Message, Message>(
           "/test.Service/Expand",
           [&](Request<Message> request,
               Stream stream) -> asio::awaitable<StreamCompletion> {
             ++calls;
             EXPECT_TRUE(
                 (co_await stream.SendHeaders({{"reply", "yes"}})).ok());
             EXPECT_TRUE((co_await stream.Write(request.message)).ok());
             EXPECT_TRUE((co_await stream.Write(request.message)).ok());
             co_return StreamCompletion{{}, {{"done", "yes"}}};
           }))
          .ok());
  ASSERT_TRUE(
      (router.RegisterClientStream<Message, Message>(
           "/test.Service/Collect",
           [&](CallContext,
               Stream stream) -> asio::awaitable<Response<Message>> {
             ++calls;
             Message output;
             for (;;) {
               auto next = co_await stream.Read();
               EXPECT_TRUE(next.ok());
               if (!next.ok() || !*next.value()) break;
               output.mutable_value()->append((**next.value()).value());
             }
             co_return Response<Message>{
                 {}, std::move(output), {{"reply", "yes"}}, {{"done", "yes"}}};
           }))
          .ok());
  ASSERT_TRUE(
      (router.RegisterBidiStream<Message, Message>(
           "/test.Service/Chat",
           [](CallContext, Stream) -> asio::awaitable<StreamCompletion> {
             ADD_FAILURE() << "HTTP/1.1 must not invoke a bidi handler";
             co_return StreamCompletion{};
           }))
          .ok());
  asio::co_spawn(loop_,
                 ServeHttp1Connection(server_,
                                      [&](std::shared_ptr<ByteStream> stream) {
                                        StreamServerOptions limits;
                                        limits.limits.max_message_bytes = 64;
                                        limits.limits.max_decompressed_bytes =
                                            4096;
                                        return router.HandleHttpStream(
                                            std::move(stream), limits);
                                      }),
                 [this](auto exception, Status status) {
                   EXPECT_FALSE(exception);
                   OnDone(std::move(status));
                 });
  StreamExchange exchange = [this](Metadata headers, TransportOptions options)
      -> asio::awaitable<Result<std::shared_ptr<ByteStream>>> {
    headers.emplace_back(":authority", "localhost");
    auto opened = co_await client_->Open(std::move(headers), options.deadline);
    if (!opened.ok())
      co_return Result<std::shared_ptr<ByteStream>>(opened.status());
    co_return Result<std::shared_ptr<ByteStream>>(*opened.value());
  };
  bool finished = false;
  auto invoke = [&]() -> asio::awaitable<void> {
    Client client({}, exchange);
    for (int encoding = 0; encoding < 3; ++encoding) {
      for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
        for (bool gzip : {false, true}) {
          Message input;
          input.set_value("hello");
          CallOptions unary;
          unary.codec = codec;
          unary.compress = gzip;
          unary.protocol =
              encoding == 0 ? Protocol::kConnect : Protocol::kGrpcWeb;
          unary.grpc_web_text = encoding == 2;
          auto response = co_await client.Unary<Message, Message>(
              "/test.Service/Echo", input, unary);
          EXPECT_TRUE(response.status.ok()) << response.status.message;
          if (!response.status.ok()) {
            co_await client_->Cancel();
            co_return;
          }
          EXPECT_EQ(response.message->value(), "hello");
          EXPECT_EQ(response.headers, (Metadata{{"reply", "yes"}}));
          EXPECT_EQ(response.trailers, (Metadata{{"done", "yes"}}));
          if (encoding == 0) {
            unary.use_get = true;
            auto get_response = co_await client.Unary<Message, Message>(
                "/test.Service/Echo", input, unary,
                {Idempotency::kNoSideEffects});
            EXPECT_TRUE(get_response.status.ok())
                << get_response.status.message;
            if (get_response.message)
              EXPECT_EQ(get_response.message->value(), "hello");
            EXPECT_EQ(get_response.headers, (Metadata{{"reply", "yes"}}));
            EXPECT_EQ(get_response.trailers, (Metadata{{"done", "yes"}}));
          }
          for (bool collect : {false, true}) {
            StreamCallOptions options;
            options.codec = codec;
            options.compress = gzip;
            options.accept_gzip = gzip;
            options.protocol = unary.protocol;
            options.grpc_web_text = unary.grpc_web_text;
            Result<Stream> opened;
            if (collect)
              opened = co_await client.ClientStream<Message, Message>(
                  "/test.Service/Collect", options);
            else
              opened = co_await client.ServerStream<Message, Message>(
                  "/test.Service/Expand", input, options);
            EXPECT_TRUE(opened.ok()) << opened.status().message;
            if (!opened.ok()) {
              co_await client_->Cancel();
              co_return;
            }
            auto stream = std::move(*opened.value());
            if (collect) {
              EXPECT_TRUE((co_await stream.Write(input)).ok());
              EXPECT_TRUE((co_await stream.Write(input)).ok());
              EXPECT_TRUE((co_await stream.CloseSend()).ok());
            }
            auto headers = co_await stream.ReadHeaders();
            EXPECT_TRUE(headers.ok()) << headers.status().message;
            if (headers.ok())
              EXPECT_EQ(*headers.value(), (Metadata{{"reply", "yes"}}));
            int count = 0;
            for (;;) {
              auto next = co_await stream.Read();
              EXPECT_TRUE(next.ok()) << next.status().message;
              if (!next.ok() || !*next.value()) break;
              EXPECT_EQ((**next.value()).value(),
                        collect ? "hellohello" : "hello");
              ++count;
            }
            EXPECT_EQ(count, collect ? 1 : 2);
            auto status = co_await stream.Finish();
            EXPECT_TRUE(status.status.ok()) << status.status.message;
            EXPECT_EQ(status.trailers, (Metadata{{"done", "yes"}}));
          }
        }
      }
    }
    // A compressed GET can be smaller than the wire limit but larger after
    // decompression. The two limits must remain independent.
    CallOptions get_options;
    get_options.use_get = true;
    get_options.compress = true;
    Message larger;
    larger.set_value(std::string(2000, 'x'));
    auto expanded = co_await client.Unary<Message, Message>(
        "/test.Service/Echo", larger, get_options,
        {Idempotency::kNoSideEffects});
    EXPECT_TRUE(expanded.status.ok()) << expanded.status.message;
    if (expanded.message) EXPECT_EQ(expanded.message->value(), larger.value());
    larger.set_value(std::string(5000, 'x'));
    auto oversized = co_await client.Unary<Message, Message>(
        "/test.Service/Echo", larger, get_options,
        {Idempotency::kNoSideEffects});
    EXPECT_EQ(oversized.status.code, Code::kResourceExhausted);
    // Rejection is a proper HTTP response and never invokes the bidi handler.
    auto raw =
        co_await client_->Open({{":method", "POST"},
                                {":authority", "localhost"},
                                {":path", "/test.Service/Chat"},
                                {"content-type", "application/connect+proto"},
                                {"connect-protocol-version", "1"}});
    EXPECT_TRUE(raw.ok());
    if (raw.ok()) {
      EXPECT_TRUE((co_await (*raw.value())->CloseSend()).ok());
      auto headers = co_await (*raw.value())->ReadHeaders();
      EXPECT_TRUE(headers.ok());
      if (headers.ok())
        EXPECT_EQ(*headers.value(), (Metadata{{":status", "505"}}));
      EXPECT_TRUE((co_await H1ReadAll(*raw.value())).ok());
      EXPECT_TRUE((co_await (*raw.value())->Finish()).ok());
    }
    finished = true;
    co_await client_->Cancel();
  };
  Spawn(invoke());
  Run();
  EXPECT_TRUE(finished);
  EXPECT_EQ(calls, 41);
}
INSTANTIATE_TEST_SUITE_P(TcpAndTls, Http1ServingTest, testing::Bool());

INSTANTIATE_TEST_SUITE_P(TcpAndTls, Http1ConnectionTest, testing::Bool());
}  // namespace
}  // namespace connectrpc
