#include "connectrpc/http2_connection.h"

#include <algorithm>
#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <memory>
#include <string>

#include "absl/strings/escaping.h"
#include "connectrpc/grpc_protocol.h"
#include "connectrpc/grpc_web_protocol.h"
#include "connectrpc/message_stream.h"
#include "connectrpc/rpc.h"
#include "connectrpc/tls.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"

namespace connectrpc {
namespace {
Metadata RequestHeaders() {
  return {{":method", "POST"},
          {":scheme", "http"},
          {":authority", "localhost"},
          {":path", "/test"}};
}
asio::awaitable<Status> WriteAndClose(std::shared_ptr<Http2ByteStream> stream,
                                      std::string bytes,
                                      Metadata trailers = {}) {
  Status status = co_await stream->Write(std::move(bytes));
  if (!status.ok()) co_return status;
  co_return co_await stream->CloseSend(std::move(trailers));
}
asio::awaitable<Result<std::string>> ReadAll(
    std::shared_ptr<Http2ByteStream> stream) {
  std::string result;
  for (;;) {
    auto next = co_await stream->Read();
    if (!next.ok()) co_return Result<std::string>(next.status());
    if (!*next.value()) break;
    result.append(**next.value());
  }
  co_return Result<std::string>(std::move(result));
}
class ConnectionTest : public testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    asio::error_code error;
    asio::ip::tcp::acceptor acceptor(loop_);
    acceptor.open(asio::ip::tcp::v4(), error);
    ASSERT_FALSE(error);
    acceptor.bind({asio::ip::address_v4::loopback(), 0}, error);
    ASSERT_FALSE(error);
    acceptor.listen(1, error);
    ASSERT_FALSE(error);
    auto endpoint = acceptor.local_endpoint(error);
    ASSERT_FALSE(error);
    asio::ip::tcp::socket client_socket(loop_);
    client_socket.connect(endpoint, error);
    ASSERT_FALSE(error);
    asio::ip::tcp::socket server_socket(loop_);
    acceptor.accept(server_socket, error);
    ASSERT_FALSE(error);
    Http2StreamOptions options;
    options.max_queued_bytes = 8192;
    Result<std::shared_ptr<Http2Connection>> client;
    Result<std::shared_ptr<Http2Connection>> server;
    if (GetParam()) {
      TlsOptions tls;
      tls.certificate_file = CONNECTCPP_TEST_CERT;
      tls.private_key_file = CONNECTCPP_TEST_KEY;
      ASSERT_TRUE(ConfigureTlsContext(server_tls_, true, tls).ok());
      tls = {};
      tls.trust_file = CONNECTCPP_TEST_CERT;
      ASSERT_TRUE(ConfigureTlsContext(client_tls_, false, tls).ok());
      auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(3);
      client = Http2Connection::CreateTls(
          asio::ssl::stream<asio::ip::tcp::socket>(std::move(client_socket),
                                                   client_tls_),
          Http2Session::Role::kClient, "localhost", options, deadline);
      server = Http2Connection::CreateTls(
          asio::ssl::stream<asio::ip::tcp::socket>(std::move(server_socket),
                                                   server_tls_),
          Http2Session::Role::kServer, "", options, deadline);
    } else {
      client = Http2Connection::Create(std::move(client_socket),
                                       Http2Session::Role::kClient, options);
      server = Http2Connection::Create(std::move(server_socket),
                                       Http2Session::Role::kServer, options);
    }
    ASSERT_TRUE(client.ok());
    ASSERT_TRUE(server.ok());
    client_ = *client.value();
    server_ = *server.value();
    watchdog_.expires_after(std::chrono::seconds(5));
    watchdog_.async_wait([this](asio::error_code error) {
      if (!error) {
        ADD_FAILURE() << "connection operations stalled";
        loop_.stop();
      }
    });
    asio::co_spawn(loop_, client_->Run(),
                   [this](auto exception, Status status) {
                     OnRunDone(exception != nullptr, std::move(status));
                   });
    if (run_server_)
      asio::co_spawn(loop_, server_->Run(),
                     [this](auto exception, Status status) {
                       OnRunDone(exception != nullptr, std::move(status));
                     });
  }
  void OnRunDone(bool exception, Status status) {
    EXPECT_FALSE(exception);
    outcomes_.push_back(std::move(status));
    if (outcomes_.size() == 2) watchdog_.cancel();
  }
  void Run() {
    loop_.run();
    EXPECT_EQ(outcomes_.size(), 2);
  }
  StreamExchange Exchange() {
    return [this](Metadata headers, TransportOptions options)
               -> asio::awaitable<Result<std::shared_ptr<ByteStream>>> {
      headers.emplace(headers.begin(), ":scheme",
                      GetParam() ? "https" : "http");
      headers.emplace(headers.begin(), ":authority", "localhost");
      auto stream =
          co_await client_->Open(std::move(headers), options.deadline);
      if (!stream.ok())
        co_return Result<std::shared_ptr<ByteStream>>(stream.status());
      co_return Result<std::shared_ptr<ByteStream>>(*stream.value());
    };
  }
  asio::io_context loop_;
  asio::ssl::context client_tls_{asio::ssl::context::tls_client};
  asio::ssl::context server_tls_{asio::ssl::context::tls_server};
  asio::steady_timer watchdog_{loop_};
  std::shared_ptr<Http2Connection> client_;
  std::shared_ptr<Http2Connection> server_;
  std::vector<Status> outcomes_;
  bool run_server_ = true;
};
TEST_P(ConnectionTest, CompleteResponsePreservesBodyTrailersAndBackpressure) {
  int completed = 0;
  bool response_queued = false;
  auto serve = [&]() -> asio::awaitable<void> {
    for (size_t size : {size_t{0}, size_t{58}, size_t{256 * 1024}}) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      auto stream = *accepted.value();
      auto request_body = co_await ReadAll(stream);
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
    }
  };
  auto invoke = [&]() -> asio::awaitable<void> {
    for (size_t size : {size_t{0}, size_t{58}, size_t{256 * 1024}}) {
      response_queued = false;
      auto opened = co_await client_->Open(RequestHeaders());
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
      auto body = co_await ReadAll(stream);
      EXPECT_TRUE(body.ok()) << body.status().message;
      if (body.ok()) EXPECT_EQ(*body.value(), std::string(size, 'r'));
      auto finish = co_await stream->Finish();
      EXPECT_TRUE(finish.ok()) << finish.status().message;
      if (finish.ok())
        EXPECT_EQ(*finish.value(), (Metadata{{"complete", "yes"}}));
      ++completed;
    }
    co_await client_->Cancel();
    co_await server_->Cancel();
  };
  asio::co_spawn(loop_, serve(), [](auto error) { EXPECT_FALSE(error); });
  asio::co_spawn(loop_, invoke(), [](auto error) { EXPECT_FALSE(error); });
  Run();
  EXPECT_EQ(completed, 3);
}
TEST_P(ConnectionTest, ConnectGetCodecsAndCompression) {
  using Message = google::protobuf::StringValue;
  Server router;
  int calls = 0;
  ASSERT_TRUE(
      (router.RegisterUnary<Message, Message>(
           "/test.Service/Read",
           [&](Request<Message> request) -> asio::awaitable<Response<Message>> {
             ++calls;
             co_return Response<Message>{{},
                                         std::move(request.message),
                                         {{"get", "yes"}},
                                         {{"done", "yes"}}};
           },
           {Idempotency::kNoSideEffects}))
          .ok());
  auto serve = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 4; ++i) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      Status status = co_await router.HandleHttpStream(*accepted.value());
      EXPECT_TRUE(status.ok()) << status.message;
    }
  };
  bool finished = false;
  auto invoke = [&]() -> asio::awaitable<void> {
    Client client({}, Exchange());
    for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
      for (bool gzip : {false, true}) {
        CallOptions options;
        options.codec = codec;
        options.compress = gzip;
        options.use_get = true;
        Message input;
        input.set_value("GET ? & + / λ");
        auto response = co_await client.Unary<Message, Message>(
            "/test.Service/Read", input, options,
            {Idempotency::kNoSideEffects});
        EXPECT_TRUE(response.status.ok()) << response.status.message;
        if (response.message)
          EXPECT_EQ(response.message->value(), input.value());
        EXPECT_EQ(response.headers, (Metadata{{"get", "yes"}}));
        EXPECT_EQ(response.trailers, (Metadata{{"done", "yes"}}));
      }
    }
    finished = true;
    co_await client_->Cancel();
    co_await server_->Cancel();
  };
  asio::co_spawn(loop_, serve(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, invoke(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(finished);
  EXPECT_EQ(calls, 4);
}
TEST_P(ConnectionTest, DuplexSlowReaderHalfCloseAndTrailers) {
  bool client_written = false;
  bool server_written = false;
  bool server_done = false;
  bool client_done = false;
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    auto stream = *accepted.value();
    auto headers = co_await stream->ReadHeaders();
    EXPECT_TRUE(headers.ok());
    Metadata response_headers{{":status", "200"}};
    auto sent = co_await stream->SendHeaders(std::move(response_headers));
    EXPECT_TRUE(sent.ok());
    asio::co_spawn(
        loop_,
        WriteAndClose(stream, std::string(256 * 1024, 's'), {{"cost", "7"}}),
        [&](auto exception, Status status) {
          EXPECT_FALSE(exception);
          EXPECT_TRUE(status.ok()) << status.message;
          server_written = true;
        });
    asio::steady_timer delay(co_await asio::this_coro::executor);
    delay.expires_after(std::chrono::milliseconds(50));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(client_written) << "write should wait for receiver capacity";
    auto bytes = co_await ReadAll(stream);
    EXPECT_TRUE(bytes.ok()) << bytes.status().message;
    if (bytes.ok()) EXPECT_EQ(*bytes.value(), std::string(256 * 1024, 'c'));
    auto finished = co_await stream->Finish();
    EXPECT_TRUE(finished.ok()) << finished.status().message;
    server_done = true;
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(RequestHeaders());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto stream = *opened.value();
    asio::co_spawn(loop_, WriteAndClose(stream, std::string(256 * 1024, 'c')),
                   [&](auto exception, Status status) {
                     EXPECT_FALSE(exception);
                     EXPECT_TRUE(status.ok()) << status.message;
                     client_written = true;
                   });
    auto headers = co_await stream->ReadHeaders();
    EXPECT_TRUE(headers.ok());
    auto bytes = co_await ReadAll(stream);
    EXPECT_TRUE(bytes.ok()) << bytes.status().message;
    if (bytes.ok()) EXPECT_EQ(*bytes.value(), std::string(256 * 1024, 's'));
    auto finished = co_await stream->Finish();
    EXPECT_TRUE(finished.ok()) << finished.status().message;
    if (finished.ok()) EXPECT_EQ(*finished.value(), (Metadata{{"cost", "7"}}));
    client_done = true;
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(client_written);
  EXPECT_TRUE(server_written);
  EXPECT_TRUE(client_done);
  EXPECT_TRUE(server_done);
}
TEST_P(ConnectionTest, DeadlineCancelsBlockedWriteAndRead) {
  std::shared_ptr<Http2ByteStream> held;
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    held = *accepted.value();
    EXPECT_TRUE((co_await held->SendHeaders({{":status", "200"}})).ok());
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(RequestHeaders());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto stream = *opened.value();
    EXPECT_TRUE((co_await stream->ReadHeaders()).ok());
    EXPECT_TRUE((co_await stream->SetDeadline(std::chrono::steady_clock::now() +
                                              std::chrono::milliseconds(100)))
                    .ok());
    Status status = co_await stream->Write(std::string(256 * 1024, 'x'));
    EXPECT_EQ(status.code, Code::kDeadlineExceeded);
    auto next = co_await stream->Read();
    EXPECT_EQ(next.status().code, Code::kDeadlineExceeded);
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  held.reset();
  loop_.restart();
  loop_.poll();
}
TEST_P(ConnectionTest, ShutdownDeadlineTerminatesUnfinishedStreams) {
  std::shared_ptr<Http2ByteStream> held;
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    held = *accepted.value();
    EXPECT_TRUE((co_await held->SendHeaders({{":status", "200"}})).ok());
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(RequestHeaders());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto stream = *opened.value();
    EXPECT_TRUE((co_await stream->ReadHeaders()).ok());
    auto status = co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                             std::chrono::milliseconds(100));
    EXPECT_EQ(status.code, Code::kDeadlineExceeded);
    auto bytes = co_await stream->Read();
    EXPECT_EQ(bytes.status().code, Code::kDeadlineExceeded);
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  held.reset();
  loop_.restart();
  loop_.poll();
}
asio::awaitable<Result<std::optional<std::string>>> ReadAfterCanceledWait(
    std::shared_ptr<Http2ByteStream> stream) {
  co_await asio::this_coro::throw_if_cancelled(false);
  asio::steady_timer timer(co_await asio::this_coro::executor);
  timer.expires_after(std::chrono::seconds(10));
  auto [error] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
  EXPECT_EQ(error, asio::error::operation_aborted);
  co_return co_await stream->Read();
}
TEST_P(ConnectionTest, CancellationBeforeReadStillResetsPeer) {
  bool reset_observed = false;
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    EXPECT_TRUE(
        (co_await (*accepted.value())->SendHeaders({{":status", "200"}})).ok());
    EXPECT_TRUE(
        (co_await (*accepted.value())->Write("buffered before cancellation"))
            .ok());
    auto response = co_await (*accepted.value())->Read();
    EXPECT_EQ(response.status().code, Code::kCanceled);
    reset_observed = true;
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(RequestHeaders());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto stream = *opened.value();
    EXPECT_TRUE((co_await stream->ReadHeaders()).ok());
    asio::cancellation_signal signal;
    asio::experimental::channel<void(asio::error_code)> done(loop_, 1);
    asio::co_spawn(
        loop_, ReadAfterCanceledWait(stream),
        asio::bind_cancellation_slot(
            signal.slot(),
            [&](auto exception, Result<std::optional<std::string>> result) {
              EXPECT_FALSE(exception);
              EXPECT_EQ(result.status().code, Code::kCanceled);
              done.try_send(asio::error_code{});
            }));
    asio::steady_timer delay(co_await asio::this_coro::executor);
    delay.expires_after(std::chrono::milliseconds(30));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    signal.emit(asio::cancellation_type::all);
    co_await done.async_receive(asio::as_tuple(asio::use_awaitable));
    auto canceled_read = co_await stream->Read();
    EXPECT_EQ(canceled_read.status().code, Code::kCanceled);
    auto finished = co_await stream->Finish();
    EXPECT_EQ(finished.status().code, Code::kCanceled);
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(reset_observed);
}
TEST_P(ConnectionTest, RejectsOverlappingReadsAndPropagatesCancellation) {
  std::shared_ptr<Http2ByteStream> held;
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    held = *accepted.value();
    EXPECT_TRUE((co_await held->SendHeaders({{":status", "200"}})).ok());
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(RequestHeaders());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto stream = *opened.value();
    EXPECT_TRUE((co_await stream->ReadHeaders()).ok());
    asio::cancellation_signal signal;
    asio::experimental::channel<void(asio::error_code)> done(loop_, 1);
    asio::co_spawn(
        loop_, stream->Read(),
        asio::bind_cancellation_slot(
            signal.slot(),
            [&](auto exception, Result<std::optional<std::string>> result) {
              EXPECT_FALSE(exception);
              EXPECT_EQ(result.status().code, Code::kCanceled);
              done.try_send(asio::error_code{});
            }));
    asio::steady_timer delay(co_await asio::this_coro::executor);
    delay.expires_after(std::chrono::milliseconds(30));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    auto overlap = co_await stream->ReadHeaders();
    EXPECT_EQ(overlap.status().code, Code::kFailedPrecondition);
    signal.emit(asio::cancellation_type::all);
    co_await done.async_receive(asio::as_tuple(asio::use_awaitable));
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  held.reset();
  loop_.restart();
  loop_.poll();
}
TEST_P(ConnectionTest, DroppingHandleBeforeReadStartsCancelsSafely) {
  auto client_operation = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(RequestHeaders());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto stream = std::move(*opened.value());
    auto pending = stream->Read();
    stream.reset();
    auto result = co_await std::move(pending);
    EXPECT_EQ(result.status().code, Code::kCanceled);
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
}
TEST_P(ConnectionTest, TypedConnectDuplexWithFinalStatus) {
  using Message = google::protobuf::StringValue;
  using Stream = TypedStream<Message, Message>;
  int completed = 0;
  auto server_operation = [&]() -> asio::awaitable<void> {
    for (int call = 0; call < 4; ++call) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      auto headers = co_await (*accepted.value())->ReadHeaders();
      EXPECT_TRUE(headers.ok());
      if (!headers.ok()) co_return;
      auto request = DecodeStreamingRequest(*headers.value());
      EXPECT_TRUE(request.ok());
      if (!request.ok()) co_return;
      StreamingResponse response{
          request.value()->codec, {{"served", "cpp"}}, request.value()->gzip};
      auto messages = ConnectMessageStream::Server(*accepted.value(),
                                                   *request.value(), response);
      EXPECT_TRUE(messages.ok());
      if (!messages.ok()) co_return;
      Stream stream(*messages.value(), request.value()->codec);
      EXPECT_TRUE((co_await stream.SendHeaders()).ok());
      int count = 0;
      for (;;) {
        auto next = co_await stream.Read();
        EXPECT_TRUE(next.ok()) << next.status().message;
        if (!next.ok()) co_return;
        if (!*next.value()) break;
        EXPECT_EQ((**next.value()).value(), std::string(70000, 'a' + count));
        EXPECT_TRUE((co_await stream.Write(**next.value())).ok());
        ++count;
      }
      EXPECT_EQ(count, 4);
      StreamCompletion completion{
          {Code::kResourceExhausted, "quota", {{"test.Detail", "abc"}}},
          {{"count", "4"}}};
      EXPECT_TRUE((co_await stream.End(completion)).ok());
      EXPECT_TRUE((co_await stream.Finish()).status.ok());
      ++completed;
    }
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
      for (bool gzip : {false, true}) {
        StreamingRequest request;
        request.procedure = "/test.Service/Duplex";
        request.codec = codec;
        request.gzip = gzip;
        auto headers = EncodeStreamingRequest(request);
        EXPECT_TRUE(headers.ok());
        if (!headers.ok()) co_return;
        headers.value()->emplace(headers.value()->begin(), ":scheme",
                                 GetParam() ? "https" : "http");
        headers.value()->emplace(headers.value()->begin(), ":authority",
                                 "localhost");
        auto bytes = co_await client_->Open(std::move(*headers.value()));
        EXPECT_TRUE(bytes.ok());
        if (!bytes.ok()) co_return;
        auto messages = ConnectMessageStream::Client(*bytes.value(), request);
        EXPECT_TRUE(messages.ok());
        if (!messages.ok()) co_return;
        Stream stream(*messages.value(), codec);
        EXPECT_EQ((co_await stream.Finish()).status.code,
                  Code::kFailedPrecondition);
        asio::experimental::channel<void(asio::error_code, Status)> written(
            loop_, 1);
        auto write = [&stream]() -> asio::awaitable<Status> {
          for (int i = 0; i < 4; ++i) {
            Message message;
            message.set_value(std::string(70000, 'a' + i));
            Status status = co_await stream.Write(message);
            if (!status.ok()) co_return status;
          }
          co_return co_await stream.CloseSend();
        };
        asio::co_spawn(loop_, write(), [&](auto exception, Status status) {
          EXPECT_FALSE(exception);
          EXPECT_TRUE(written.try_send(asio::error_code{}, std::move(status)));
        });
        auto response_headers = co_await stream.ReadHeaders();
        EXPECT_TRUE(response_headers.ok());
        if (response_headers.ok())
          EXPECT_EQ(*response_headers.value(), (Metadata{{"served", "cpp"}}));
        int count = 0;
        for (;;) {
          auto next = co_await stream.Read();
          EXPECT_TRUE(next.ok()) << next.status().message;
          if (!next.ok()) break;
          if (!*next.value()) break;
          EXPECT_EQ((**next.value()).value(), std::string(70000, 'a' + count));
          ++count;
        }
        EXPECT_EQ(count, 4);
        auto [error, sent] =
            co_await written.async_receive(asio::as_tuple(asio::use_awaitable));
        EXPECT_FALSE(error);
        EXPECT_TRUE(sent.ok()) << sent.message;
        auto finished = co_await stream.Finish();
        EXPECT_EQ(finished.status.code, Code::kResourceExhausted);
        EXPECT_EQ(finished.status.message, "quota");
        EXPECT_EQ(finished.trailers, (Metadata{{"count", "4"}}));
        EXPECT_EQ(finished.status.details.size(), 1);
        Message late;
        EXPECT_EQ((co_await stream.Write(late)).code,
                  Code::kFailedPrecondition);
      }
    }
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_EQ(completed, 4);
}
TEST_P(ConnectionTest, ConnectRejectsMissingTerminal) {
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    auto stream = *accepted.value();
    EXPECT_TRUE((co_await stream->SendHeaders(
                     {{":status", "200"},
                      {"content-type", "application/connect+proto"}}))
                    .ok());
    EXPECT_TRUE((co_await stream->CloseSend()).ok());
    EXPECT_TRUE((co_await stream->Read()).ok());
    EXPECT_TRUE((co_await stream->Finish()).ok());
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    StreamingRequest request;
    request.procedure = "/test.Service/Stream";
    auto headers = EncodeStreamingRequest(request);
    headers.value()->emplace(headers.value()->begin(), ":scheme", "http");
    headers.value()->emplace(headers.value()->begin(), ":authority",
                             "localhost");
    auto bytes = co_await client_->Open(std::move(*headers.value()));
    EXPECT_TRUE(bytes.ok());
    if (!bytes.ok()) co_return;
    auto messages = ConnectMessageStream::Client(*bytes.value(), request);
    EXPECT_TRUE(messages.ok());
    if (!messages.ok()) co_return;
    auto stream = *messages.value();
    EXPECT_TRUE((co_await stream->CloseSend()).ok());
    EXPECT_EQ((co_await stream->Read()).status().code, Code::kInternal);
    EXPECT_EQ((co_await stream->Finish()).status.code, Code::kInternal);
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
}
TEST_P(ConnectionTest, ConnectCancellationWakesBothDirections) {
  std::shared_ptr<Http2ByteStream> held;
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    held = *accepted.value();
    EXPECT_TRUE((co_await held->SendHeaders(
                     {{":status", "200"},
                      {"content-type", "application/connect+proto"}}))
                    .ok());
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    StreamingRequest request;
    request.procedure = "/test.Service/Stream";
    auto headers = EncodeStreamingRequest(request);
    headers.value()->emplace(headers.value()->begin(), ":scheme", "http");
    headers.value()->emplace(headers.value()->begin(), ":authority",
                             "localhost");
    auto bytes = co_await client_->Open(std::move(*headers.value()));
    EXPECT_TRUE(bytes.ok());
    if (!bytes.ok()) co_return;
    auto messages = ConnectMessageStream::Client(*bytes.value(), request);
    EXPECT_TRUE(messages.ok());
    if (!messages.ok()) co_return;
    auto stream = *messages.value();
    EXPECT_TRUE((co_await stream->ReadHeaders()).ok());
    asio::experimental::channel<void(asio::error_code)> done(loop_, 2);
    asio::co_spawn(loop_, stream->Write(std::string(256 * 1024, 'x')),
                   [&](auto exception, Status status) {
                     EXPECT_FALSE(exception);
                     EXPECT_EQ(status.code, Code::kCanceled);
                     EXPECT_TRUE(done.try_send(asio::error_code{}));
                   });
    asio::co_spawn(
        loop_, stream->Read(),
        [&](auto exception, Result<std::optional<std::string>> result) {
          EXPECT_FALSE(exception);
          EXPECT_EQ(result.status().code, Code::kCanceled);
          EXPECT_TRUE(done.try_send(asio::error_code{}));
        });
    asio::steady_timer delay(loop_);
    delay.expires_after(std::chrono::milliseconds(30));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    EXPECT_EQ((co_await stream->Read()).status().code,
              Code::kFailedPrecondition);
    EXPECT_EQ((co_await stream->Write("overlap")).code,
              Code::kFailedPrecondition);
    EXPECT_TRUE((co_await stream->Cancel()).ok());
    for (int i = 0; i < 2; ++i) {
      auto [error] =
          co_await done.async_receive(asio::as_tuple(asio::use_awaitable));
      EXPECT_FALSE(error);
    }
    EXPECT_EQ((co_await stream->Finish()).status.code, Code::kCanceled);
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  held.reset();
  loop_.restart();
  loop_.poll();
}
TEST_P(ConnectionTest, ConnectCallerCancellationBeforeRead) {
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    auto stream = *accepted.value();
    EXPECT_TRUE((co_await stream->SendHeaders(
                     {{":status", "200"},
                      {"content-type", "application/connect+proto"}}))
                    .ok());
    auto message = EncodeStreamingMessage("buffered", false);
    auto terminal = EncodeEndStream({});
    EXPECT_TRUE(
        (co_await stream->Write(*message.value() + *terminal.value())).ok());
    EXPECT_TRUE((co_await stream->CloseSend()).ok());
    EXPECT_TRUE((co_await stream->Read()).ok());
    EXPECT_TRUE((co_await stream->Finish()).ok());
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    StreamingRequest request;
    request.procedure = "/test.Service/Stream";
    auto headers = EncodeStreamingRequest(request);
    headers.value()->emplace(headers.value()->begin(), ":scheme", "http");
    headers.value()->emplace(headers.value()->begin(), ":authority",
                             "localhost");
    auto bytes = co_await client_->Open(std::move(*headers.value()));
    EXPECT_TRUE(bytes.ok());
    if (!bytes.ok()) co_return;
    auto messages = ConnectMessageStream::Client(*bytes.value(), request);
    EXPECT_TRUE(messages.ok());
    if (!messages.ok()) co_return;
    auto stream = *messages.value();
    EXPECT_TRUE((co_await stream->CloseSend()).ok());
    EXPECT_TRUE((co_await stream->ReadHeaders()).ok());
    asio::experimental::channel<void(asio::error_code)> done(loop_, 1);
    asio::cancellation_signal cancel;
    auto read = [&]() -> asio::awaitable<void> {
      co_await asio::this_coro::throw_if_cancelled(false);
      asio::steady_timer delay(loop_);
      delay.expires_after(std::chrono::hours(1));
      co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
      EXPECT_EQ((co_await stream->Read()).status().code, Code::kCanceled);
    };
    asio::co_spawn(
        loop_, read(),
        asio::bind_cancellation_slot(cancel.slot(), [&](auto exception) {
          EXPECT_FALSE(exception);
          EXPECT_TRUE(done.try_send(asio::error_code{}));
        }));
    asio::steady_timer delay(loop_);
    delay.expires_after(std::chrono::milliseconds(30));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    cancel.emit(asio::cancellation_type::terminal);
    auto [error] =
        co_await done.async_receive(asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(error);
    EXPECT_EQ((co_await stream->Finish()).status.code, Code::kCanceled);
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
}
TEST_P(ConnectionTest, ConnectEarlyResponseUnblocksRequestWriter) {
  std::shared_ptr<Http2ByteStream> held;
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    held = *accepted.value();
    EXPECT_TRUE((co_await held->SendHeaders(
                     {{":status", "200"},
                      {"content-type", "application/connect+proto"}}))
                    .ok());
    asio::steady_timer delay(loop_);
    delay.expires_after(std::chrono::milliseconds(50));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    auto terminal =
        EncodeEndStream({{Code::kPermissionDenied, "denied", {}}, {}});
    EXPECT_TRUE((co_await held->Write(*terminal.value())).ok());
    EXPECT_TRUE((co_await held->CloseSend()).ok());
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    StreamingRequest request;
    request.procedure = "/test.Service/Stream";
    auto headers = EncodeStreamingRequest(request);
    headers.value()->emplace(headers.value()->begin(), ":scheme", "http");
    headers.value()->emplace(headers.value()->begin(), ":authority",
                             "localhost");
    auto bytes = co_await client_->Open(std::move(*headers.value()));
    EXPECT_TRUE(bytes.ok());
    if (!bytes.ok()) co_return;
    auto messages = ConnectMessageStream::Client(*bytes.value(), request);
    EXPECT_TRUE(messages.ok());
    if (!messages.ok()) co_return;
    auto stream = *messages.value();
    asio::experimental::channel<void(asio::error_code)> done(loop_, 1);
    asio::co_spawn(loop_, stream->Write(std::string(256 * 1024, 'x')),
                   [&](auto exception, Status status) {
                     EXPECT_FALSE(exception);
                     EXPECT_EQ(status.code, Code::kCanceled);
                     EXPECT_TRUE(done.try_send(asio::error_code{}));
                   });
    auto eof = co_await stream->Read();
    EXPECT_TRUE(eof.ok()) << eof.status().message;
    if (eof.ok()) EXPECT_FALSE(*eof.value());
    EXPECT_EQ((co_await stream->Finish()).status.code, Code::kPermissionDenied);
    auto [error] =
        co_await done.async_receive(asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(error);
    EXPECT_EQ((co_await stream->Finish()).status.code, Code::kPermissionDenied);
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  held.reset();
  loop_.restart();
  loop_.poll();
}
TEST_P(ConnectionTest, MalformedRequestReceivesTerminalBeforeReset) {
  int rejected = 0;
  auto server_operation = [&]() -> asio::awaitable<void> {
    for (int mode = 0; mode < 2; ++mode) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      auto headers = co_await (*accepted.value())->ReadHeaders();
      EXPECT_TRUE(headers.ok());
      if (!headers.ok()) co_return;
      auto request = DecodeStreamingRequest(*headers.value());
      EXPECT_TRUE(request.ok());
      if (!request.ok()) co_return;
      auto messages =
          ConnectMessageStream::Server(*accepted.value(), *request.value(), {});
      EXPECT_TRUE(messages.ok());
      if (!messages.ok()) co_return;
      TypedStream<google::protobuf::StringValue, google::protobuf::StringValue>
          stream(*messages.value(), Codec::kProtobuf);
      auto input = co_await stream.Read();
      EXPECT_EQ(input.status().code, Code::kInvalidArgument);
      EXPECT_EQ((co_await stream.Read()).status().code, Code::kInvalidArgument);
      EXPECT_TRUE((co_await stream.End({{}, {{"rejected", "yes"}}})).ok());
      EXPECT_TRUE((co_await stream.Finish()).status.ok());
      ++rejected;
    }
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    for (int mode = 0; mode < 2; ++mode) {
      StreamingRequest request;
      request.procedure = "/test.Service/Stream";
      auto headers = EncodeStreamingRequest(request);
      headers.value()->emplace(headers.value()->begin(), ":scheme", "http");
      headers.value()->emplace(headers.value()->begin(), ":authority",
                               "localhost");
      auto bytes = co_await client_->Open(std::move(*headers.value()));
      EXPECT_TRUE(bytes.ok());
      if (!bytes.ok()) co_return;
      auto messages = ConnectMessageStream::Client(*bytes.value(), request);
      EXPECT_TRUE(messages.ok());
      if (!messages.ok()) co_return;
      auto stream = *messages.value();
      asio::experimental::channel<void(asio::error_code)> done(loop_, 1);
      std::string malformed =
          mode == 0
              ? std::string("\4\0\0\0\0", 5)
              : *EncodeStreamingMessage(std::string("\xff", 1), false).value();
      auto write = [&, mode]() -> asio::awaitable<Status> {
        if (mode == 0)
          co_return co_await (*bytes.value())
              ->Write(malformed + std::string(256 * 1024, 'x'));
        Status sent = co_await stream->Write(std::string("\xff", 1));
        if (!sent.ok()) co_return sent;
        co_return co_await stream->Write(std::string(256 * 1024, 'x'));
      };
      asio::co_spawn(loop_, write(), [&](auto exception, Status status) {
        EXPECT_FALSE(exception);
        EXPECT_FALSE(status.ok());
        EXPECT_TRUE(done.try_send(asio::error_code{}));
      });
      auto eof = co_await stream->Read();
      EXPECT_TRUE(eof.ok()) << eof.status().message;
      if (eof.ok()) EXPECT_FALSE(*eof.value());
      auto completion = co_await stream->Finish();
      EXPECT_EQ(completion.status.code, Code::kInvalidArgument);
      EXPECT_EQ(completion.trailers, (Metadata{{"rejected", "yes"}}));
      auto [error] =
          co_await done.async_receive(asio::as_tuple(asio::use_awaitable));
      EXPECT_FALSE(error);
    }
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_EQ(rejected, 2);
}
TEST_P(ConnectionTest, ResponseFlushWaitsForFlowControl) {
  bool flushed = false;
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    auto stream = *accepted.value();
    EXPECT_EQ((co_await stream->FinishSend()).code, Code::kFailedPrecondition);
    EXPECT_EQ((co_await stream->DiscardRead()).code, Code::kFailedPrecondition);
    EXPECT_TRUE((co_await stream->SendHeaders({{":status", "200"}})).ok());
    EXPECT_TRUE((co_await stream->Write(std::string(100000, 'x'))).ok());
    EXPECT_TRUE((co_await stream->CloseSend({{"done", "yes"}})).ok());
    EXPECT_TRUE((co_await stream->FinishSend()).ok());
    flushed = true;
    EXPECT_TRUE((co_await stream->DiscardRead()).ok());
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(RequestHeaders());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto stream = *opened.value();
    EXPECT_TRUE((co_await stream->ReadHeaders()).ok());
    asio::steady_timer delay(loop_);
    delay.expires_after(std::chrono::milliseconds(30));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(flushed);
    auto result = co_await ReadAll(stream);
    EXPECT_TRUE(result.ok()) << result.status().message;
    if (result.ok()) EXPECT_EQ(*result.value(), std::string(100000, 'x'));
    auto finished = co_await stream->Finish();
    EXPECT_TRUE(finished.ok());
    if (finished.ok())
      EXPECT_EQ(*finished.value(), (Metadata{{"done", "yes"}}));
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(flushed);
}
TEST_P(ConnectionTest, ClientStreamingMethodsEnforceShapes) {
  using Message = google::protobuf::StringValue;
  using Stream = TypedStream<Message, Message>;
  constexpr StreamShape kShapes[] = {StreamShape::kServerStreaming,
                                     StreamShape::kClientStreaming,
                                     StreamShape::kBidiStreaming};
  auto server_operation = [&]() -> asio::awaitable<void> {
    for (StreamShape shape : kShapes) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      auto headers = co_await (*accepted.value())->ReadHeaders();
      EXPECT_TRUE(headers.ok());
      if (!headers.ok()) co_return;
      auto request = DecodeStreamingRequest(*headers.value());
      EXPECT_TRUE(request.ok());
      if (!request.ok()) co_return;
      request.value()->shape = shape;
      EXPECT_EQ(request.value()->headers, (Metadata{{"request-key", "value"}}));
      auto messages =
          ConnectMessageStream::Server(*accepted.value(), *request.value(), {});
      EXPECT_TRUE(messages.ok());
      if (!messages.ok()) co_return;
      Stream stream(*messages.value(), Codec::kProtobuf);
      int count = 0;
      for (;;) {
        auto next = co_await stream.Read();
        EXPECT_TRUE(next.ok()) << next.status().message;
        if (!next.ok()) co_return;
        if (!*next.value()) break;
        EXPECT_EQ((**next.value()).value(), "input");
        ++count;
      }
      EXPECT_EQ(count, shape == StreamShape::kServerStreaming ? 1 : 2);
      int responses = shape == StreamShape::kClientStreaming ? 1 : 2;
      for (int i = 0; i < responses; ++i) {
        Message response;
        response.set_value("output");
        EXPECT_TRUE((co_await stream.Write(response)).ok());
      }
      EXPECT_TRUE((co_await stream.End({})).ok());
      EXPECT_TRUE((co_await stream.Finish()).status.ok());
    }
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    StreamExchange exchange = [this](Metadata headers, TransportOptions options)
        -> asio::awaitable<Result<std::shared_ptr<ByteStream>>> {
      headers.emplace(headers.begin(), ":scheme",
                      GetParam() ? "https" : "http");
      headers.emplace(headers.begin(), ":authority", "localhost");
      auto stream =
          co_await client_->Open(std::move(headers), options.deadline);
      if (!stream.ok())
        co_return Result<std::shared_ptr<ByteStream>>(stream.status());
      co_return Result<std::shared_ptr<ByteStream>>(*stream.value());
    };
    for (StreamShape shape : kShapes) {
      Message input;
      input.set_value("input");
      StreamCallOptions options;
      options.headers = {{"request-key", "value"}};
      options.deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      // The returned operation owns its exchange, even when Client is destroyed
      // before the coroutine starts.
      auto make_call = [&]() {
        Client client({}, exchange);
        if (shape == StreamShape::kServerStreaming)
          return client.ServerStream<Message, Message>("/test.Service/Stream",
                                                       input, options);
        if (shape == StreamShape::kClientStreaming)
          return client.ClientStream<Message, Message>("/test.Service/Stream",
                                                       options);
        return client.BidiStream<Message, Message>("/test.Service/Stream",
                                                   options);
      };
      auto opened = co_await make_call();
      EXPECT_TRUE(opened.ok()) << opened.status().message;
      if (!opened.ok()) co_return;
      Stream stream = std::move(*opened.value());
      if (shape != StreamShape::kServerStreaming) {
        EXPECT_TRUE((co_await stream.Write(input)).ok());
        EXPECT_TRUE((co_await stream.Write(input)).ok());
        EXPECT_TRUE((co_await stream.CloseSend()).ok());
      } else {
        EXPECT_EQ((co_await stream.Write(input)).code,
                  Code::kFailedPrecondition);
      }
      int count = 0;
      for (;;) {
        auto next = co_await stream.Read();
        EXPECT_TRUE(next.ok()) << next.status().message;
        if (!next.ok()) break;
        if (!*next.value()) break;
        EXPECT_EQ((**next.value()).value(), "output");
        ++count;
      }
      EXPECT_EQ(count, shape == StreamShape::kClientStreaming ? 1 : 2);
      EXPECT_TRUE((co_await stream.Finish()).status.ok());
    }
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
}
TEST_P(ConnectionTest, ClientStreamingRejectsMissingAndExtraResponses) {
  auto server_operation = [&]() -> asio::awaitable<void> {
    for (int count : {0, 2}) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      auto bytes = *accepted.value();
      EXPECT_TRUE((co_await bytes->SendHeaders(
                       {{":status", "200"},
                        {"content-type", "application/connect+proto"}}))
                      .ok());
      for (int i = 0; i < count; ++i) {
        auto message = EncodeStreamingMessage("", false);
        EXPECT_TRUE((co_await bytes->Write(*message.value())).ok());
      }
      auto terminal = EncodeEndStream({});
      EXPECT_TRUE((co_await bytes->Write(*terminal.value())).ok());
      EXPECT_TRUE((co_await bytes->CloseSend()).ok());
      EXPECT_TRUE((co_await bytes->FinishSend()).ok());
      EXPECT_TRUE((co_await bytes->DiscardRead()).ok());
    }
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    for (int count : {0, 2}) {
      StreamingRequest request;
      request.procedure = "/test.Service/Stream";
      request.shape = StreamShape::kClientStreaming;
      auto headers = EncodeStreamingRequest(request);
      headers.value()->emplace(headers.value()->begin(), ":scheme", "http");
      headers.value()->emplace(headers.value()->begin(), ":authority",
                               "localhost");
      auto bytes = co_await client_->Open(std::move(*headers.value()));
      EXPECT_TRUE(bytes.ok());
      if (!bytes.ok()) co_return;
      auto messages = ConnectMessageStream::Client(*bytes.value(), request);
      EXPECT_TRUE(messages.ok());
      if (!messages.ok()) co_return;
      auto stream = *messages.value();
      EXPECT_TRUE((co_await stream->CloseSend()).ok());
      if (count == 2) EXPECT_TRUE((co_await stream->Read()).ok());
      EXPECT_EQ((co_await stream->Read()).status().code, Code::kUnimplemented);
      EXPECT_EQ((co_await stream->Finish()).status.code, Code::kUnimplemented);
    }
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
}
TEST_P(ConnectionTest, ServerStreamingOpenPreservesEarlyRpcError) {
  using Message = google::protobuf::StringValue;
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    auto headers = co_await (*accepted.value())->ReadHeaders();
    EXPECT_TRUE(headers.ok());
    if (!headers.ok()) co_return;
    auto request = DecodeStreamingRequest(*headers.value());
    EXPECT_TRUE(request.ok());
    if (!request.ok()) co_return;
    request.value()->shape = StreamShape::kServerStreaming;
    auto messages =
        ConnectMessageStream::Server(*accepted.value(), *request.value(), {});
    EXPECT_TRUE(messages.ok());
    if (!messages.ok()) co_return;
    auto stream = *messages.value();
    EXPECT_TRUE(
        (co_await stream->End(
             {{Code::kPermissionDenied, "rejected before reading", {}}, {}}))
            .ok());
    EXPECT_TRUE((co_await stream->Finish()).status.ok());
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    Client client(
        {},
        [this](Metadata headers, TransportOptions options)
            -> asio::awaitable<Result<std::shared_ptr<ByteStream>>> {
          headers.emplace(headers.begin(), ":scheme", "http");
          headers.emplace(headers.begin(), ":authority", "localhost");
          auto stream =
              co_await client_->Open(std::move(headers), options.deadline);
          if (!stream.ok())
            co_return Result<std::shared_ptr<ByteStream>>(stream.status());
          co_return Result<std::shared_ptr<ByteStream>>(*stream.value());
        });
    Message input;
    input.set_value(std::string(256 * 1024, 'x'));
    auto opened = co_await client.ServerStream<Message, Message>(
        "/test.Service/Stream", input);
    EXPECT_TRUE(opened.ok()) << opened.status().message;
    if (!opened.ok()) {
      co_await client_->Cancel();
      co_return;
    }
    auto next = co_await opened.value()->Read();
    EXPECT_TRUE(next.ok()) << next.status().message;
    if (next.ok()) EXPECT_FALSE(*next.value());
    auto finished = co_await opened.value()->Finish();
    EXPECT_EQ(finished.status.code, Code::kPermissionDenied);
    EXPECT_EQ(finished.status.message, "rejected before reading");
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
}
TEST_P(ConnectionTest, FinishSendDeadlineWakesBlockedFlush) {
  asio::experimental::channel<void(asio::error_code)> server_done(loop_, 1);
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    auto stream = *accepted.value();
    EXPECT_TRUE((co_await stream->SendHeaders({{":status", "200"}})).ok());
    // One full stream window plus one queue fits Write but cannot all reach the
    // wire until the peer returns credit. CloseSend and FinishSend must wait.
    EXPECT_TRUE((co_await stream->Write(std::string(65535 + 8192, 'x'))).ok());
    EXPECT_TRUE((co_await stream->CloseSend()).ok());
    EXPECT_TRUE((co_await stream->SetDeadline(std::chrono::steady_clock::now() +
                                              std::chrono::milliseconds(50)))
                    .ok());
    EXPECT_EQ((co_await stream->FinishSend()).code, Code::kDeadlineExceeded);
    EXPECT_TRUE(server_done.try_send(asio::error_code{}));
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    auto opened = co_await client_->Open(RequestHeaders());
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    EXPECT_TRUE((co_await (*opened.value())->ReadHeaders()).ok());
    auto [error] =
        co_await server_done.async_receive(asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(error);
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
}
TEST_P(ConnectionTest, RegisteredStreamingMethodsRoundTrip) {
  using Message = google::protobuf::StringValue;
  using Stream = TypedStream<Message, Message>;
  Server router;
  int called = 0;
  ASSERT_TRUE(
      (router.RegisterServerStream<Message, Message>(
           "/test.Service/Expand",
           [&](Request<Message> request,
               Stream stream) -> asio::awaitable<StreamCompletion> {
             ++called;
             EXPECT_EQ(request.headers, (Metadata{{"input-key", "yes"}}));
             EXPECT_EQ(request.message.value(), "input");
             EXPECT_TRUE(
                 (co_await stream.SendHeaders({{"output-key", "yes"}})).ok());
             EXPECT_EQ((co_await stream.SendHeaders({{"late", "no"}})).code,
                       Code::kFailedPrecondition);
             for (int i = 0; i < 2; ++i) {
               Message message;
               message.set_value("output");
               EXPECT_TRUE((co_await stream.Write(message)).ok());
             }
             co_return StreamCompletion{{}, {{"done", "yes"}}};
           }))
          .ok());
  ASSERT_TRUE((router.RegisterClientStream<Message, Message>(
                   "/test.Service/Collect",
                   [&](CallContext context,
                       Stream stream) -> asio::awaitable<Response<Message>> {
                     ++called;
                     EXPECT_EQ(context.headers,
                               (Metadata{{"input-key", "yes"}}));
                     int count = 0;
                     for (;;) {
                       auto next = co_await stream.Read();
                       EXPECT_TRUE(next.ok());
                       if (!next.ok() || !*next.value()) break;
                       EXPECT_EQ((**next.value()).value(), "input");
                       ++count;
                     }
                     EXPECT_EQ(count, 2);
                     Message message;
                     message.set_value("output");
                     co_return Response<Message>{{},
                                                 std::move(message),
                                                 {{"output-key", "yes"}},
                                                 {{"done", "yes"}}};
                   }))
                  .ok());
  ASSERT_TRUE(
      (router.RegisterBidiStream<Message, Message>(
           "/test.Service/Chat",
           [&](CallContext context,
               Stream stream) -> asio::awaitable<StreamCompletion> {
             ++called;
             EXPECT_EQ(context.headers, (Metadata{{"input-key", "yes"}}));
             EXPECT_TRUE(
                 (co_await stream.SendHeaders({{"output-key", "yes"}})).ok());
             for (;;) {
               auto next = co_await stream.Read();
               EXPECT_TRUE(next.ok());
               if (!next.ok() || !*next.value()) break;
               Message message;
               message.set_value("output");
               EXPECT_TRUE((co_await stream.Write(message)).ok());
             }
             co_return StreamCompletion{{}, {{"done", "yes"}}};
           }))
          .ok());
  EXPECT_EQ(router.CanRegister("/test.Service/Chat").code,
            Code::kAlreadyExists);
  EXPECT_EQ((router.RegisterUnary<Message, Message>(
                 "/test.Service/Chat",
                 [](Request<Message>) -> asio::awaitable<Response<Message>> {
                   co_return Response<Message>{};
                 }))
                .code,
            Code::kAlreadyExists);
  auto server_operation = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 48; ++i) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      Status status = co_await router.HandleStream(*accepted.value());
      EXPECT_TRUE(status.ok()) << status.message;
    }
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    Client client({}, Exchange());
    for (int encoding = 0; encoding < 4; ++encoding) {
      Protocol protocol = encoding == 0   ? Protocol::kConnect
                          : encoding == 1 ? Protocol::kGrpc
                                          : Protocol::kGrpcWeb;
      for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
        for (bool gzip : {false, true}) {
          for (int shape = 0; shape < 3; ++shape) {
            StreamCallOptions options;
            options.protocol = protocol;
            options.grpc_web_text = encoding == 3;
            options.codec = codec;
            options.compress = gzip;
            options.accept_gzip = gzip;
            options.headers = {{"input-key", "yes"}};
            Message input;
            input.set_value("input");
            Result<Stream> opened;
            if (shape == 0)
              opened = co_await client.ServerStream<Message, Message>(
                  "/test.Service/Expand", input, options);
            else if (shape == 1)
              opened = co_await client.ClientStream<Message, Message>(
                  "/test.Service/Collect", options);
            else
              opened = co_await client.BidiStream<Message, Message>(
                  "/test.Service/Chat", options);
            EXPECT_TRUE(opened.ok());
            if (!opened.ok()) co_return;
            auto stream = std::move(*opened.value());
            if (shape != 0) {
              EXPECT_TRUE((co_await stream.Write(input)).ok());
              EXPECT_TRUE((co_await stream.Write(input)).ok());
              EXPECT_TRUE((co_await stream.CloseSend()).ok());
            }
            auto headers = co_await stream.ReadHeaders();
            EXPECT_TRUE(headers.ok());
            if (headers.ok())
              EXPECT_EQ(*headers.value(), (Metadata{{"output-key", "yes"}}));
            int count = 0;
            for (;;) {
              auto next = co_await stream.Read();
              EXPECT_TRUE(next.ok()) << next.status().message;
              if (!next.ok() || !*next.value()) break;
              EXPECT_EQ((**next.value()).value(), "output");
              ++count;
            }
            EXPECT_EQ(count, shape == 1 ? 1 : 2);
            auto finished = co_await stream.Finish();
            EXPECT_TRUE(finished.status.ok()) << finished.status.message;
            EXPECT_EQ(finished.trailers, (Metadata{{"done", "yes"}}));
          }
        }
      }
    }
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_EQ(called, 48);
}
TEST_P(ConnectionTest, RoutingRejectsMalformedSingleRequestsBeforeHandler) {
  using Message = google::protobuf::StringValue;
  Server router;
  int called = 0;
  ASSERT_TRUE((router.RegisterServerStream<Message, Message>(
                   "/test.Service/Expand",
                   [&](Request<Message>, TypedStream<Message, Message>)
                       -> asio::awaitable<StreamCompletion> {
                     ++called;
                     co_return StreamCompletion{};
                   }))
                  .ok());
  auto server_operation = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 5; ++i) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      Status status = co_await router.HandleStream(*accepted.value());
      EXPECT_TRUE(status.ok()) << status.message;
    }
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    for (int mode = 0; mode < 5; ++mode) {
      StreamingRequest request;
      request.procedure =
          mode == 4 ? "/unknown.Service/Missing" : "/test.Service/Expand";
      auto headers = EncodeStreamingRequest(request);
      auto opened = co_await Exchange()(std::move(*headers.value()), {});
      EXPECT_TRUE(opened.ok());
      if (!opened.ok()) co_return;
      auto transport = *opened.value();
      std::string bytes;
      if (mode == 1) {
        bytes = *EncodeStreamingMessage("", false).value();
        bytes += bytes;
      } else if (mode == 2) {
        bytes = *EncodeStreamingMessage("", false).value();
        bytes.append(1, '\0');
      } else if (mode == 3) {
        bytes = *EncodeStreamingMessage(std::string(1, '\xff'), false).value();
      }
      EXPECT_TRUE((co_await transport->Write(std::move(bytes))).ok());
      EXPECT_TRUE((co_await transport->CloseSend({})).ok());
      auto created = ConnectMessageStream::Client(transport, request);
      EXPECT_TRUE(created.ok());
      if (!created.ok()) co_return;
      auto stream = *created.value();
      auto eof = co_await stream->Read();
      EXPECT_TRUE(eof.ok()) << eof.status().message;
      if (eof.ok()) EXPECT_FALSE(*eof.value());
      auto finished = co_await stream->Finish();
      EXPECT_EQ(finished.status.code, (mode == 0 || mode == 1 || mode == 4)
                                          ? Code::kUnimplemented
                                          : Code::kInvalidArgument);
    }
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_EQ(called, 0);
}
TEST_P(ConnectionTest, StreamingHandlerDeadlineJoinsBeforeTerminalResponse) {
  using Message = google::protobuf::StringValue;
  Server router;
  bool joined = false;
  ASSERT_TRUE(
      (router.RegisterClientStream<Message, Message>(
           "/test.Service/Wait",
           [&](CallContext context, TypedStream<Message, Message>)
               -> asio::awaitable<Response<Message>> {
             EXPECT_LT(context.deadline,
                       std::chrono::steady_clock::time_point::max());
             co_await asio::this_coro::throw_if_cancelled(false);
             asio::steady_timer delay(co_await asio::this_coro::executor);
             delay.expires_after(std::chrono::hours(1));
             auto [error] =
                 co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
             EXPECT_EQ(error, asio::error::operation_aborted);
             joined = true;
             // Ignoring cancellation in the application result must not make
             // the adapter write a successful response on the canceled task.
             Message output;
             output.set_value("late result");
             co_return Response<Message>{
                 {}, std::move(output), {}, {{"joined", "yes"}}};
           }))
          .ok());
  auto server_operation = [&]() -> asio::awaitable<void> {
    auto accepted = co_await server_->Accept();
    EXPECT_TRUE(accepted.ok());
    if (!accepted.ok()) co_return;
    Status status = co_await router.HandleStream(*accepted.value());
    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_TRUE(joined);
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    StreamingRequest request;
    request.procedure = "/test.Service/Wait";
    auto headers = EncodeStreamingRequest(request);
    headers.value()->emplace_back("connect-timeout-ms", "30");
    auto opened = co_await Exchange()(std::move(*headers.value()), {});
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto created = ConnectMessageStream::Client(*opened.value(), request);
    EXPECT_TRUE(created.ok());
    if (!created.ok()) co_return;
    auto stream = *created.value();
    auto eof = co_await stream->Read();
    EXPECT_TRUE(eof.ok()) << eof.status().message;
    if (eof.ok()) EXPECT_FALSE(*eof.value());
    auto finished = co_await stream->Finish();
    EXPECT_EQ(finished.status.code, Code::kDeadlineExceeded);
    EXPECT_EQ(finished.trailers, (Metadata{{"joined", "yes"}}));
    EXPECT_TRUE(joined);
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
}
TEST_P(ConnectionTest, StreamingDeadlineInterruptsIncompleteRequest) {
  using Message = google::protobuf::StringValue;
  using Stream = TypedStream<Message, Message>;
  Server router;
  int called = 0;
  ASSERT_TRUE((router.RegisterBidiStream<Message, Message>(
                   "/test.Service/Read",
                   [&](CallContext,
                       Stream stream) -> asio::awaitable<StreamCompletion> {
                     ++called;
                     auto next = co_await stream.Read();
                     EXPECT_EQ(next.status().code, Code::kCanceled);
                     co_return StreamCompletion{};
                   }))
                  .ok());
  ASSERT_TRUE(
      (router.RegisterServerStream<Message, Message>(
           "/test.Service/Expand",
           [&](Request<Message>, Stream) -> asio::awaitable<StreamCompletion> {
             ++called;
             co_return StreamCompletion{};
           }))
          .ok());
  auto server_operation = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 2; ++i) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      Status status = co_await router.HandleStream(*accepted.value());
      EXPECT_TRUE(status.ok()) << status.message;
    }
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    for (std::string procedure :
         {"/test.Service/Read", "/test.Service/Expand"}) {
      StreamingRequest request;
      request.procedure = std::move(procedure);
      auto headers = EncodeStreamingRequest(request);
      headers.value()->emplace_back("connect-timeout-ms", "30");
      auto opened = co_await Exchange()(std::move(*headers.value()), {});
      EXPECT_TRUE(opened.ok());
      if (!opened.ok()) co_return;
      // Leave an incomplete envelope with the request direction still open.
      EXPECT_TRUE(
          (co_await (*opened.value())->Write(std::string(1, '\0'))).ok());
      auto created = ConnectMessageStream::Client(*opened.value(), request);
      EXPECT_TRUE(created.ok());
      if (!created.ok()) co_return;
      auto eof = co_await (*created.value())->Read();
      EXPECT_TRUE(eof.ok()) << eof.status().message;
      if (eof.ok()) EXPECT_FALSE(*eof.value());
      EXPECT_EQ((co_await (*created.value())->Finish()).status.code,
                Code::kDeadlineExceeded);
    }
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_EQ(called, 1);
}
TEST_P(ConnectionTest, StreamingPeerClosureCancelsIdleHandlerAndJoins) {
  using Message = google::protobuf::StringValue;
  Server router;
  int joined = 0;
  asio::experimental::channel<void(asio::error_code)> started(loop_, 1);
  asio::experimental::channel<void(asio::error_code)> completed(loop_, 1);
  ASSERT_TRUE(
      (router.RegisterBidiStream<Message, Message>(
           "/test.Service/Idle",
           [&](CallContext, TypedStream<Message, Message>)
               -> asio::awaitable<StreamCompletion> {
             co_await asio::this_coro::throw_if_cancelled(false);
             EXPECT_TRUE(started.try_send(asio::error_code{}));
             asio::steady_timer delay(co_await asio::this_coro::executor);
             delay.expires_after(std::chrono::hours(1));
             auto [error] =
                 co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
             EXPECT_EQ(error, asio::error::operation_aborted);
             ++joined;
             co_return StreamCompletion{};
           }))
          .ok());
  auto server_operation = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 2; ++i) {
      auto accepted = co_await server_->Accept();
      EXPECT_TRUE(accepted.ok());
      if (!accepted.ok()) co_return;
      Status status = co_await router.HandleStream(*accepted.value());
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(joined, i + 1);
      EXPECT_TRUE(completed.try_send(asio::error_code{}));
    }
  };
  auto client_operation = [&]() -> asio::awaitable<void> {
    Client client({}, Exchange());
    for (int i = 0; i < 2; ++i) {
      auto opened =
          co_await client.BidiStream<Message, Message>("/test.Service/Idle");
      EXPECT_TRUE(opened.ok());
      if (!opened.ok()) co_return;
      auto [start_error] =
          co_await started.async_receive(asio::as_tuple(asio::use_awaitable));
      EXPECT_FALSE(start_error);
      if (i == 0)
        EXPECT_TRUE((co_await opened.value()->Cancel()).ok());
      else
        EXPECT_TRUE((co_await client_->Cancel()).ok());
      auto [done_error] =
          co_await completed.async_receive(asio::as_tuple(asio::use_awaitable));
      EXPECT_FALSE(done_error);
    }
  };
  asio::co_spawn(loop_, server_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_EQ(joined, 2);
}
class ServingTest : public ConnectionTest {
 protected:
  void SetUp() override {
    run_server_ = false;
    ConnectionTest::SetUp();
  }
  void Start(HttpStreamHandler handler, size_t max_handlers,
             asio::cancellation_slot slot = {}) {
    asio::co_spawn(
        loop_, ServeHttp2Connection(server_, std::move(handler), max_handlers),
        asio::bind_cancellation_slot(
            slot, [this](auto exception, Status status) {
              OnRunDone(exception != nullptr, std::move(status));
            }));
  }
};
TEST_P(ServingTest, CountsCanceledHandlersUntilTheyJoin) {
  using Message = google::protobuf::StringValue;
  asio::experimental::channel<void(asio::error_code)> started(loop_, 1);
  asio::experimental::channel<void(asio::error_code)> canceled(loop_, 1);
  asio::experimental::channel<void(asio::error_code)> release(loop_, 1);
  int handlers_started = 0;
  int handlers_joined = 0;
  Server router;
  ASSERT_TRUE((router.RegisterBidiStream<Message, Message>(
                   "/test.Service/Wait",
                   [&](CallContext, TypedStream<Message, Message>)
                       -> asio::awaitable<StreamCompletion> {
                     co_await asio::this_coro::throw_if_cancelled(false);
                     if (++handlers_started == 1) {
                       EXPECT_TRUE(started.try_send(asio::error_code{}));
                       asio::steady_timer delay(
                           co_await asio::this_coro::executor);
                       delay.expires_after(std::chrono::hours(1));
                       auto [error] = co_await delay.async_wait(
                           asio::as_tuple(asio::use_awaitable));
                       EXPECT_EQ(error, asio::error::operation_aborted);
                       EXPECT_TRUE(canceled.try_send(asio::error_code{}));
                       co_await asio::this_coro::reset_cancellation_state(
                           asio::disable_cancellation());
                       co_await release.async_receive(
                           asio::as_tuple(asio::use_awaitable));
                     }
                     ++handlers_joined;
                     co_return StreamCompletion{};
                   }))
                  .ok());
  Start(
      [&](std::shared_ptr<ByteStream> stream) {
        return router.HandleHttpStream(std::move(stream));
      },
      1);
  auto client_operation = [&]() -> asio::awaitable<void> {
    Client client({}, Exchange());
    auto first =
        co_await client.BidiStream<Message, Message>("/test.Service/Wait");
    EXPECT_TRUE(first.ok());
    if (!first.ok()) co_return;
    co_await started.async_receive(asio::as_tuple(asio::use_awaitable));
    EXPECT_TRUE((co_await first.value()->Cancel()).ok());
    co_await canceled.async_receive(asio::as_tuple(asio::use_awaitable));
    auto second =
        co_await client.BidiStream<Message, Message>("/test.Service/Wait");
    EXPECT_TRUE(second.ok());
    if (!second.ok()) co_return;
    asio::steady_timer delay(loop_);
    delay.expires_after(std::chrono::milliseconds(20));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    EXPECT_EQ(handlers_started, 1);
    EXPECT_EQ(handlers_joined, 0);
    EXPECT_TRUE(release.try_send(asio::error_code{}));
    auto eof = co_await second.value()->Read();
    EXPECT_TRUE(eof.ok()) << eof.status().message;
    if (eof.ok()) EXPECT_FALSE(*eof.value());
    EXPECT_TRUE((co_await second.value()->Finish()).status.ok());
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_EQ(handlers_started, 2);
  EXPECT_EQ(handlers_joined, 2);
}
TEST_P(ServingTest, UnaryAndBidiShareConnectionWithIndependentResponseLimits) {
  using Message = google::protobuf::StringValue;
  asio::experimental::channel<void(asio::error_code)> unary_started(loop_, 1);
  asio::experimental::channel<void(asio::error_code)> release_unary(loop_, 1);
  asio::experimental::channel<void(asio::error_code)> unary_done(loop_, 1);
  Server router;
  bool unary_completed = false;
  ASSERT_TRUE(
      (router.RegisterUnary<Message, Message>(
           "/test.Service/Unary",
           [&](Request<Message> request) -> asio::awaitable<Response<Message>> {
             EXPECT_EQ(request.message.value(), "input");
             unary_started.try_send(asio::error_code{});
             co_await release_unary.async_receive(
                 asio::as_tuple(asio::use_awaitable));
             Message output;
             output.set_value(std::string(20000, 'u'));
             co_return Response<Message>{{}, std::move(output), {}, {}};
           }))
          .ok());
  ASSERT_TRUE((router.RegisterBidiStream<Message, Message>(
                   "/test.Service/Bidi",
                   [](CallContext, TypedStream<Message, Message> stream)
                       -> asio::awaitable<StreamCompletion> {
                     auto request = co_await stream.Read();
                     EXPECT_TRUE(request.ok());
                     if (!request.ok())
                       co_return StreamCompletion{request.status(), {}};
                     Message output;
                     output.set_value(std::string(20000, 's'));
                     co_return StreamCompletion{co_await stream.Write(output),
                                                {}};
                   }))
                  .ok());
  Start(
      [&](std::shared_ptr<ByteStream> stream) {
        StreamServerOptions options;
        options.limits.max_message_bytes = 8;
        options.limits.max_decompressed_bytes = 8;
        options.send_limits.max_message_bytes = 32000;
        options.send_limits.max_decompressed_bytes = 32000;
        return router.HandleHttpStream(std::move(stream), options);
      },
      2);
  Client client({}, Exchange());
  Message input;
  input.set_value("input");
  asio::co_spawn(
      loop_, client.Unary<Message, Message>("/test.Service/Unary", input),
      [&](auto exception, Response<Message> response) {
        EXPECT_FALSE(exception);
        EXPECT_TRUE(response.status.ok()) << response.status.message;
        if (response.message)
          EXPECT_EQ(response.message->value(), std::string(20000, 'u'));
        else
          ADD_FAILURE() << "missing unary response";
        unary_completed = true;
        unary_done.try_send(asio::error_code{});
      });
  auto client_operation = [&]() -> asio::awaitable<void> {
    co_await unary_started.async_receive(asio::as_tuple(asio::use_awaitable));
    auto opened =
        co_await client.BidiStream<Message, Message>("/test.Service/Bidi");
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) co_return;
    auto stream = *opened.value();
    EXPECT_TRUE((co_await stream.Write(input)).ok());
    EXPECT_TRUE((co_await stream.CloseSend()).ok());
    auto next = co_await stream.Read();
    EXPECT_TRUE(next.ok()) << next.status().message;
    if (next.ok() && *next.value())
      EXPECT_EQ((**next.value()).value(), std::string(20000, 's'));
    else
      ADD_FAILURE() << "missing bidi response";
    auto eof = co_await stream.Read();
    EXPECT_TRUE(eof.ok());
    if (eof.ok()) EXPECT_FALSE(*eof.value());
    EXPECT_TRUE((co_await stream.Finish()).status.ok());
    EXPECT_FALSE(unary_completed);
    release_unary.try_send(asio::error_code{});
    co_await unary_done.async_receive(asio::as_tuple(asio::use_awaitable));
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
  };
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(unary_completed);
}
TEST_P(ServingTest, RpcCancellationAtTransportHandoff) {
  using Message = google::protobuf::StringValue;
  Server router;
  ASSERT_TRUE(
      (router.RegisterUnary<Message, Message>(
           "/test.Service/Unary",
           [](Request<Message> request) -> asio::awaitable<Response<Message>> {
             Response<Message> response;
             response.message = std::move(request.message);
             co_return response;
           }))
          .ok());
  Start(
      [&](std::shared_ptr<ByteStream> stream) {
        return router.HandleHttpStream(std::move(stream));
      },
      4);
  bool done = false;
  auto operation = [&]() -> asio::awaitable<void> {
    for (Protocol protocol :
         {Protocol::kConnect, Protocol::kGrpc, Protocol::kGrpcWeb}) {
      for (int shape = 0; shape < 4; ++shape) {
        auto signal = std::make_shared<asio::cancellation_signal>();
        auto exchange = Exchange();
        Client client(
            {},
            [signal, exchange](Metadata headers, TransportOptions options)
                -> asio::awaitable<Result<std::shared_ptr<ByteStream>>> {
              auto opened = co_await exchange(std::move(headers), options);
              // Deliberately cancel after transport success, before the RPC
              // reader task installs its slot. Cancellation signals are not
              // sticky.
              signal->emit(asio::cancellation_type::all);
              co_return opened;
            });
        CallOptions options;
        options.protocol = protocol;
        Message input;
        input.set_value("input");
        auto call = [&]() -> asio::awaitable<Status> {
          if (shape == 0) {
            auto response = co_await client.Unary<Message, Message>(
                "/test.Service/Unary", input, options);
            EXPECT_FALSE(response.message);
            co_return response.status;
          }
          StreamCallOptions streaming;
          streaming.protocol = protocol;
          Result<TypedStream<Message, Message>> opened;
          if (shape == 1)
            opened = co_await client.ServerStream<Message, Message>(
                "/test.Service/Unary", input, streaming);
          else if (shape == 2)
            opened = co_await client.ClientStream<Message, Message>(
                "/test.Service/Unary", streaming);
          else
            opened = co_await client.BidiStream<Message, Message>(
                "/test.Service/Unary", streaming);
          Status status = opened.status();
          if (opened.ok()) co_await opened.value()->Cancel();
          co_return status;
        };
        auto [exception, status] = co_await asio::co_spawn(
            loop_, call(),
            asio::bind_cancellation_slot(signal->slot(),
                                         asio::as_tuple(asio::use_awaitable)));
        EXPECT_FALSE(exception);
        EXPECT_EQ(status.code, Code::kCanceled);
      }
    }
    Client healthy({}, Exchange());
    CallOptions options;
    options.protocol = Protocol::kGrpc;
    auto response = co_await healthy.Unary<Message, Message>(
        "/test.Service/Unary", Message{}, options);
    EXPECT_TRUE(response.status.ok()) << response.status.message;
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
    done = true;
  };
  asio::co_spawn(loop_, operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(done);
}
TEST_P(ServingTest, GrpcUnaryCodecsCompressionAndErrors) {
  using Message = google::protobuf::StringValue;
  Server router;
  int calls = 0;
  ASSERT_TRUE(
      (router.RegisterUnary<Message, Message>(
           "/test.Service/Unary",
           [&](Request<Message> request) -> asio::awaitable<Response<Message>> {
             ++calls;
             EXPECT_EQ(request.headers, (Metadata{{"request-key", "yes"}}));
             EXPECT_GT(request.deadline, std::chrono::steady_clock::now());
             Response<Message> response;
             response.headers = {{"response-key", "yes"}};
             response.trailers = {{"cost", "1"}, {"cost", "2"}};
             if (request.message.value() == "error")
               response.status = {Code::kPermissionDenied,
                                  "no % access\n",
                                  {{"example.Detail", "detail"}}};
             else
               response.message = std::move(request.message);
             co_return response;
           }))
          .ok());
  Start(
      [&](std::shared_ptr<ByteStream> stream) {
        return router.HandleHttpStream(std::move(stream));
      },
      4);
  bool done = false;
  auto operation = [&]() -> asio::awaitable<void> {
    Client client({}, Exchange());
    for (int encoding = 0; encoding < 3; ++encoding) {
      for (Codec codec : {Codec::kProtobuf, Codec::kJson}) {
        for (bool gzip : {false, true}) {
          for (bool fail : {false, true}) {
            CallOptions options;
            options.protocol =
                encoding == 0 ? Protocol::kGrpc : Protocol::kGrpcWeb;
            options.grpc_web_text = encoding == 2;
            options.codec = codec;
            options.compress = gzip;
            options.headers = {{"request-key", "yes"}};
            options.transport.deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            Message input;
            input.set_value(fail ? "error" : std::string(128 * 1024, 'x'));
            auto response = co_await client.Unary<Message, Message>(
                "/test.Service/Unary", input, options);
            EXPECT_EQ(response.headers, (Metadata{{"response-key", "yes"}}));
            EXPECT_EQ(response.trailers,
                      (Metadata{{"cost", "1"}, {"cost", "2"}}));
            if (fail) {
              EXPECT_EQ(response.status.code, Code::kPermissionDenied);
              EXPECT_EQ(response.status.message, "no % access\n");
              EXPECT_FALSE(response.message);
              EXPECT_EQ(response.status.details.size(), 1);
              if (!response.status.details.empty())
                EXPECT_EQ(response.status.details[0].value, "detail");
            } else {
              EXPECT_TRUE(response.status.ok()) << response.status.message;
              EXPECT_TRUE(response.message);
              if (response.message)
                EXPECT_EQ(response.message->value(), input.value());
            }
          }
        }
      }
    }
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
    done = true;
  };
  asio::co_spawn(loop_, operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(done);
  EXPECT_EQ(calls, 24);
}
TEST_P(ServingTest, GrpcWebTrailersOnlyAndBodyStatusPrecedence) {
  using Message = google::protobuf::StringValue;
  int calls = 0;
  Start(
      [&](std::shared_ptr<ByteStream> stream) -> asio::awaitable<Status> {
        int mode = calls++;
        Metadata headers{
            {":status", "200"},
            {"content-type",
             mode >= 2 ? "application/grpc-web-text" : "application/grpc-web"},
            {"grpc-status", mode == 0 ? "9" : "invalid"},
            {"leading", "yes"}};
        Status status = co_await stream->SendHeaders(std::move(headers));
        if (mode != 0 && status.ok()) {
          auto terminal = EncodeGrpcWebTrailers(
              {{Code::kFailedPrecondition, "end", {}}, {{"trailing", "yes"}}});
          EXPECT_TRUE(terminal.ok());
          if (!terminal.ok()) co_return terminal.status();
          std::string wire = std::move(*terminal.value());
          if (mode >= 2) {
            std::string text;
            // Padded segments inside the envelope, independently of writes.
            for (size_t i = 0; i < wire.size(); i += 2)
              text += absl::Base64Escape(std::string_view(wire).substr(i, 2));
            wire = std::move(text);
          }
          if (mode == 3) wire += "!";
          for (size_t i = 0; i < wire.size() && status.ok(); i += 3)
            status = co_await stream->Write(wire.substr(i, 3));
        }
        if (status.ok()) status = co_await stream->CloseSend({});
        if (status.ok()) status = co_await stream->FinishSend();
        if (status.ok()) status = co_await stream->DiscardRead();
        co_return status;
      },
      1);
  bool done = false;
  auto operation = [&]() -> asio::awaitable<void> {
    Client client({}, Exchange());
    for (int mode = 0; mode < 4; ++mode) {
      CallOptions options;
      options.protocol = Protocol::kGrpcWeb;
      options.grpc_web_text = mode >= 2;
      Message input;
      input.set_value(std::string(256 * 1024, 'x'));
      auto response = co_await client.Unary<Message, Message>(
          "/test.Service/Unary", input, options);
      EXPECT_EQ(response.status.code,
                mode == 3 ? Code::kInternal : Code::kFailedPrecondition);
      EXPECT_EQ(response.headers, (Metadata{{"leading", "yes"}}));
      if (mode < 3)
        EXPECT_EQ(response.trailers,
                  (mode == 0 ? Metadata{{"leading", "yes"}}
                             : Metadata{{"trailing", "yes"}}));
    }
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
    done = true;
  };
  asio::co_spawn(loop_, operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(done);
  EXPECT_EQ(calls, 4);
}
TEST_P(ServingTest, GrpcTrailersOverrideMisplacedStatusHeaders) {
  using Message = google::protobuf::StringValue;
  int calls = 0;
  Start(
      [&](std::shared_ptr<ByteStream> stream) -> asio::awaitable<Status> {
        ++calls;
        // Invalid status/details in a non-terminal initial header block must be
        // ignored. A later trailer block is authoritative, with separate
        // metadata.
        Metadata headers{{":status", "200"},
                         {"content-type", "application/grpc"},
                         {"grpc-status", "invalid"},
                         {"grpc-status-details-bin", "%"},
                         {"leading", "yes"}};
        auto status = co_await stream->SendHeaders(std::move(headers));
        if (status.ok())
          status = co_await stream->CloseSend(
              {{"grpc-status", "9"}, {"trailing", "yes"}});
        if (status.ok()) status = co_await stream->FinishSend();
        if (status.ok()) status = co_await stream->DiscardRead();
        co_return status;
      },
      1);
  bool done = false;
  auto operation = [&]() -> asio::awaitable<void> {
    Client client({}, Exchange());
    CallOptions options;
    options.protocol = Protocol::kGrpc;
    Message input;
    input.set_value(std::string(256 * 1024, 'x'));
    auto response = co_await client.Unary<Message, Message>(
        "/test.Service/Unary", input, options);
    EXPECT_EQ(response.status.code, Code::kFailedPrecondition);
    EXPECT_EQ(response.headers, (Metadata{{"leading", "yes"}}));
    EXPECT_EQ(response.trailers, (Metadata{{"trailing", "yes"}}));
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
    done = true;
  };
  asio::co_spawn(loop_, operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(done);
  EXPECT_EQ(calls, 1);
}
TEST_P(ServingTest, UnaryLifetimeAndEarlyResponseDuringBlockedUpload) {
  using Message = google::protobuf::StringValue;
  const std::string rejection(128 * 1024, 'r');
  bool flushed = false;
  Start(
      [&](std::shared_ptr<ByteStream> stream) -> asio::awaitable<Status> {
        auto headers = co_await stream->ReadHeaders();
        if (!headers.ok()) co_return headers.status();
        // Deliberately never consume the request. Both bodies exceed the
        // stream window, so a sequential upload/read client would deadlock.
        UnaryResponse response;
        response.status = {Code::kPermissionDenied, rejection, {}};
        response.headers = {{"test-header", "early"}};
        response.trailers = {{"test-trailer", "complete"}};
        auto encoded =
            EncodeUnaryResponse(std::move(response), Codec::kProtobuf, false);
        if (!encoded.ok()) co_return encoded.status();
        Status status =
            co_await stream->SendHeaders(std::move(encoded.value()->headers));
        if (status.ok())
          status = co_await stream->Write(std::move(encoded.value()->body));
        if (status.ok()) status = co_await stream->CloseSend({});
        if (status.ok()) status = co_await stream->FinishSend();
        flushed = status.ok();
        if (status.ok()) status = co_await stream->DiscardRead();
        co_return status;
      },
      1);
  bool done = false;
  auto operation = [&]() -> asio::awaitable<void> {
    auto owner = std::make_unique<Client>(UnaryExchange{}, Exchange());
    Message input;
    input.set_value(std::string(256 * 1024, 'u'));
    auto call = owner->Unary<Message, Message>("/test.Service/Unary", input);
    owner.reset();
    auto response = co_await std::move(call);
    EXPECT_EQ(response.status.code, Code::kPermissionDenied);
    EXPECT_EQ(response.status.message, rejection);
    EXPECT_FALSE(response.message);
    EXPECT_EQ(response.headers, (Metadata{{"test-header", "early"}}));
    EXPECT_EQ(response.trailers, (Metadata{{"test-trailer", "complete"}}));
    EXPECT_TRUE((co_await client_->Shutdown(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(1)))
                    .ok());
    done = true;
  };
  asio::co_spawn(loop_, operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(done);
  EXPECT_TRUE(flushed);
}
TEST_P(ServingTest, UnaryCancellationJoinsBlockedUpload) {
  using Message = google::protobuf::StringValue;
  asio::cancellation_signal cancellation;
  asio::steady_timer timer(loop_);
  bool server_closed = false;
  Start(
      [&](std::shared_ptr<ByteStream> stream) -> asio::awaitable<Status> {
        auto headers = co_await stream->ReadHeaders();
        if (!headers.ok()) co_return headers.status();
        timer.expires_after(std::chrono::milliseconds(30));
        timer.async_wait([&](asio::error_code error) {
          if (!error) cancellation.emit(asio::cancellation_type::all);
        });
        auto closed = co_await stream->WaitClosed();
        server_closed = !closed.ok();
        co_return closed;
      },
      1);
  Client client({}, Exchange());
  Message input;
  input.set_value(std::string(256 * 1024, 'u'));
  bool done = false;
  asio::co_spawn(
      loop_, client.Unary<Message, Message>("/test.Service/Unary", input),
      asio::bind_cancellation_slot(
          cancellation.slot(), [&](auto exception, Response<Message> response) {
            EXPECT_FALSE(exception);
            EXPECT_EQ(response.status.code, Code::kCanceled);
            EXPECT_FALSE(response.message);
            done = true;
            asio::co_spawn(loop_,
                           client_->Shutdown(std::chrono::steady_clock::now() +
                                             std::chrono::seconds(1)),
                           [](auto shutdown_exception, Status) {
                             EXPECT_FALSE(shutdown_exception);
                           });
          }));
  Run();
  EXPECT_TRUE(done);
  EXPECT_TRUE(server_closed);
}
TEST_P(ServingTest, ParentCancellationJoinsHandlerAtCapacity) {
  asio::experimental::channel<void(asio::error_code)> started(loop_, 1);
  asio::cancellation_signal cancel;
  bool joined = false;
  Start(
      [&](std::shared_ptr<ByteStream>) -> asio::awaitable<Status> {
        co_await asio::this_coro::throw_if_cancelled(false);
        EXPECT_TRUE(started.try_send(asio::error_code{}));
        asio::steady_timer delay(co_await asio::this_coro::executor);
        delay.expires_after(std::chrono::hours(1));
        auto [error] =
            co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
        EXPECT_EQ(error, asio::error::operation_aborted);
        joined = true;
        co_return Status{Code::kCanceled, "canceled handler", {}};
      },
      1, cancel.slot());
  auto client_operation = [&]() -> asio::awaitable<void> {
    auto stream = co_await client_->Open(RequestHeaders());
    EXPECT_TRUE(stream.ok());
    if (!stream.ok()) co_return;
    co_await started.async_receive(asio::as_tuple(asio::use_awaitable));
    cancel.emit(asio::cancellation_type::all);
    EXPECT_FALSE((co_await (*stream.value())->ReadHeaders()).ok());
  };
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(joined);
  EXPECT_TRUE(std::any_of(
      outcomes_.begin(), outcomes_.end(),
      [](const Status& status) { return status.code == Code::kCanceled; }));
}
TEST_P(ServingTest, CancellationBeforeServingStartsClosesConnection) {
  asio::cancellation_signal cancel;
  asio::experimental::channel<void(asio::error_code)> ready(loop_, 1);
  bool invoked = false;
  auto serve = [&]() -> asio::awaitable<Status> {
    co_await asio::this_coro::throw_if_cancelled(false);
    ready.try_send(asio::error_code{});
    asio::steady_timer delay(co_await asio::this_coro::executor);
    delay.expires_after(std::chrono::hours(1));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    co_return co_await ServeHttp2Connection(
        server_, [&](std::shared_ptr<ByteStream>) -> asio::awaitable<Status> {
          invoked = true;
          co_return Status{};
        });
  };
  asio::co_spawn(loop_, serve(),
                 asio::bind_cancellation_slot(
                     cancel.slot(), [this](auto exception, Status status) {
                       EXPECT_EQ(status.code, Code::kCanceled);
                       OnRunDone(exception != nullptr, std::move(status));
                     }));
  auto trigger = [&]() -> asio::awaitable<void> {
    co_await ready.async_receive(asio::as_tuple(asio::use_awaitable));
    cancel.emit(asio::cancellation_type::all);
  };
  asio::co_spawn(loop_, trigger(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_FALSE(invoked);
}
TEST_P(ServingTest, ParentCancellationInterruptsGracefulDrain) {
  asio::experimental::channel<void(asio::error_code)> started(loop_, 1);
  asio::cancellation_signal cancel;
  bool handler_joined = false;
  bool shutdown_joined = false;
  Start(
      [&](std::shared_ptr<ByteStream>) -> asio::awaitable<Status> {
        co_await asio::this_coro::throw_if_cancelled(false);
        started.try_send(asio::error_code{});
        asio::steady_timer delay(co_await asio::this_coro::executor);
        delay.expires_after(std::chrono::hours(1));
        auto [error] =
            co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
        EXPECT_EQ(error, asio::error::operation_aborted);
        handler_joined = true;
        co_return Status{Code::kCanceled, "canceled handler", {}};
      },
      2, cancel.slot());
  auto client_operation = [&]() -> asio::awaitable<void> {
    auto stream = co_await client_->Open(RequestHeaders());
    EXPECT_TRUE(stream.ok());
    if (!stream.ok()) co_return;
    co_await started.async_receive(asio::as_tuple(asio::use_awaitable));
    asio::co_spawn(loop_,
                   server_->Shutdown(std::chrono::steady_clock::now() +
                                     std::chrono::hours(1)),
                   [&](auto exception, Status) {
                     EXPECT_FALSE(exception);
                     shutdown_joined = true;
                   });
    // Let Accept observe draining while the active handler remains suspended.
    asio::steady_timer delay(loop_);
    delay.expires_after(std::chrono::milliseconds(20));
    co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    EXPECT_FALSE(handler_joined);
    cancel.emit(asio::cancellation_type::all);
    EXPECT_FALSE((co_await (*stream.value())->ReadHeaders()).ok());
  };
  asio::co_spawn(loop_, client_operation(),
                 [](auto exception) { EXPECT_FALSE(exception); });
  Run();
  EXPECT_TRUE(handler_joined);
  EXPECT_TRUE(shutdown_joined);
}
INSTANTIATE_TEST_SUITE_P(Transport, ServingTest, testing::Values(false, true));
INSTANTIATE_TEST_SUITE_P(Transport, ConnectionTest,
                         testing::Values(false, true));
}  // namespace
}  // namespace connectrpc
