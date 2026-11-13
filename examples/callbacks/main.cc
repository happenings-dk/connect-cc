#include <asio/io_context.hpp>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "connectrpc/callback.h"
#include "connectrpc/http_connection.h"
#include "connectrpc/rpc.h"
#include "google/protobuf/wrappers.pb.h"

namespace {
using Message = google::protobuf::StringValue;
using Stream = connectrpc::TypedStream<Message, Message>;
using connectrpc::Async;
using connectrpc::Status;

// One instance per RPC; shared counters are safe on this example's single
// event-loop thread. A multi-threaded application must synchronize its sink.
class TraceCall final : public connectrpc::CallInterceptor {
 public:
  TraceCall(const connectrpc::CallInfo& info, int& completed)
      : info_(info), completed_(completed) {}
  asio::awaitable<Status> OnRequest(connectrpc::Metadata& headers) override {
    if (info_.client) headers.emplace_back("trace-id", "callback-example");
    co_return Status{};
  }
  void OnComplete(const Status& status) override {
    ++completed_;
    std::cout << (info_.client ? "client " : "server ") << info_.procedure
              << " status=" << static_cast<int>(status.code) << '\n';
  }

 private:
  const connectrpc::CallInfo& info_;
  int& completed_;
};

// A callback retains the session only while its operation is pending. Each
// stream has at most one pending read and one pending write. The example uses
// one event-loop thread; use a strand if running that loop on multiple threads.
class Conversation : public std::enable_shared_from_this<Conversation> {
 public:
  Conversation(asio::any_io_executor executor, connectrpc::Client client,
               std::shared_ptr<connectrpc::HttpConnection> connection,
               std::function<void(Status)> done)
      : executor_(std::move(executor)),
        client_(std::move(client)),
        connection_(std::move(connection)),
        done_(std::move(done)) {}
  Conversation(const Conversation&) = delete;
  Conversation& operator=(const Conversation&) = delete;
  void Start() {
    Message input;
    input.set_value("hello");
    Async(executor_,
          client_.Unary<Message, Message>("/example.Echo/Say", input),
          [self = shared_from_this()](connectrpc::Response<Message> response) {
            if (!response.status.ok()) {
              self->Stop(response.status);
              return;
            }
            std::cout << "unary: " << response.message->value() << '\n';
            self->StartStream();
          });
  }

 private:
  void StartStream() {
    read_done_ = false;
    write_done_ = shape_ == 0;
    written_ = 0;
    auto opened = [self =
                       shared_from_this()](connectrpc::Result<Stream> result) {
      if (!result.ok()) {
        self->Stop(result.status());
        return;
      }
      self->stream_.emplace(std::move(*result.value()));
      self->Read();
      if (!self->write_done_) self->Write();
    };
    Message input;
    input.set_value("hello");
    if (shape_ == 0)
      Async(
          executor_,
          client_.ServerStream<Message, Message>("/example.Echo/Expand", input),
          std::move(opened));
    else if (shape_ == 1)
      Async(executor_,
            client_.ClientStream<Message, Message>("/example.Echo/Collect"),
            std::move(opened));
    else
      Async(executor_,
            client_.BidiStream<Message, Message>("/example.Echo/Chat"),
            std::move(opened));
  }
  void Read() {
    Async(executor_, stream_->Read(),
          [self = shared_from_this()](
              connectrpc::Result<std::optional<Message>> next) {
            if (self->stopping_) return;
            if (!next.ok()) {
              self->Stop(next.status());
              return;
            }
            if (*next.value()) {
              std::cout << "stream " << self->shape_ << ": "
                        << (**next.value()).value() << '\n';
              self->Read();
            } else {
              self->read_done_ = true;
              self->Finish();
            }
          });
  }
  void Write() {
    Message input;
    input.set_value("hello");
    Async(executor_, stream_->Write(input),
          [self = shared_from_this()](Status status) {
            if (self->stopping_) return;
            if (!status.ok()) {
              self->Stop(status);
              return;
            }
            if (++self->written_ < 2) {
              self->Write();
              return;
            }
            Async(self->executor_, self->stream_->CloseSend(),
                  [self](Status status) {
                    if (self->stopping_) return;
                    if (!status.ok()) {
                      self->Stop(status);
                      return;
                    }
                    self->write_done_ = true;
                    self->Finish();
                  });
          });
  }
  void Finish() {
    if (!read_done_ || !write_done_) return;
    Async(executor_, stream_->Finish(),
          [self = shared_from_this()](connectrpc::StreamCompletion result) {
            if (!result.status.ok()) {
              self->Stop(result.status);
              return;
            }
            self->stream_.reset();
            if (++self->shape_ < 3)
              self->StartStream();
            else
              self->Stop({});
          });
  }
  void Stop(Status outcome) {
    if (std::exchange(stopping_, true)) return;
    // Cleanup has no canceled slot bound to it. Cancel closes the connection;
    // the application still joins Run and the server's serving operation.
    Async(executor_, connection_->Cancel(),
          [self = shared_from_this(), outcome = std::move(outcome)](
              Status) mutable { self->done_(std::move(outcome)); });
  }
  asio::any_io_executor executor_;
  connectrpc::Client client_;
  std::shared_ptr<connectrpc::HttpConnection> connection_;
  std::function<void(Status)> done_;
  std::optional<Stream> stream_;
  int shape_ = 0;
  int written_ = 0;
  bool read_done_ = false;
  bool write_done_ = false;
  bool stopping_ = false;
};

Status Register(connectrpc::Server& server) {
  Status status = server.RegisterUnary<Message, Message>(
      "/example.Echo/Say",
      [](connectrpc::Request<Message> request)
          -> asio::awaitable<connectrpc::Response<Message>> {
        co_return connectrpc::Response<Message>{
            {}, std::move(request.message), {}, {}};
      });
  if (!status.ok()) return status;
  status = server.RegisterServerStream<Message, Message>(
      "/example.Echo/Expand",
      [](connectrpc::Request<Message> request,
         Stream stream) -> asio::awaitable<connectrpc::StreamCompletion> {
        Status status = co_await stream.Write(request.message);
        if (status.ok()) status = co_await stream.Write(request.message);
        co_return connectrpc::StreamCompletion{status, {}};
      });
  if (!status.ok()) return status;
  status = server.RegisterClientStream<Message, Message>(
      "/example.Echo/Collect",
      [](connectrpc::CallContext,
         Stream stream) -> asio::awaitable<connectrpc::Response<Message>> {
        connectrpc::Response<Message> response;
        response.message.emplace();
        for (;;) {
          auto next = co_await stream.Read();
          if (!next.ok()) {
            response.status = next.status();
            break;
          }
          if (!*next.value()) break;
          response.message->mutable_value()->append((**next.value()).value());
        }
        co_return response;
      });
  if (!status.ok()) return status;
  return server.RegisterBidiStream<Message, Message>(
      "/example.Echo/Chat",
      [](connectrpc::CallContext,
         Stream stream) -> asio::awaitable<connectrpc::StreamCompletion> {
        for (;;) {
          auto next = co_await stream.Read();
          if (!next.ok())
            co_return connectrpc::StreamCompletion{next.status(), {}};
          if (!*next.value()) co_return connectrpc::StreamCompletion{};
          Status status = co_await stream.Write(**next.value());
          if (!status.ok()) co_return connectrpc::StreamCompletion{status, {}};
        }
      });
}
}  // namespace

int main() {
  asio::io_context loop;
  asio::error_code error;
  asio::ip::tcp::acceptor acceptor(loop);
  acceptor.open(asio::ip::tcp::v4(), error);
  if (!error) acceptor.bind({asio::ip::address_v4::loopback(), 0}, error);
  if (!error) acceptor.listen(1, error);
  if (error) {
    std::cerr << error.message() << '\n';
    return 1;
  }
  auto endpoint = acceptor.local_endpoint(error);
  if (error) return 1;
  asio::ip::tcp::socket client_socket(loop), server_socket(loop);
  client_socket.connect(endpoint, error);
  if (!error) acceptor.accept(server_socket, error);
  if (error) {
    std::cerr << error.message() << '\n';
    return 1;
  }
  auto client = connectrpc::HttpConnection::Create(
      std::move(client_socket), connectrpc::HttpConnection::Role::kClient,
      connectrpc::HttpVersion::kHttp2);
  auto server = connectrpc::HttpConnection::Create(
      std::move(server_socket), connectrpc::HttpConnection::Role::kServer,
      connectrpc::HttpVersion::kHttp2);
  if (!client.ok() || !server.ok()) return 1;
  int client_calls = 0;
  int server_calls = 0;
  connectrpc::Server router({[&](const connectrpc::CallInfo& info) {
    return std::make_unique<TraceCall>(info, server_calls);
  }});
  Status registered = Register(router);
  if (!registered.ok()) return 1;
  int joined = 0;
  bool done = false;
  bool succeeded = false;
  Async(loop.get_executor(), (*client.value())->Run(),
        [&](Status) { ++joined; });
  Async(loop.get_executor(),
        connectrpc::ServeHttpConnection(
            *server.value(),
            [&](std::shared_ptr<connectrpc::ByteStream> stream) {
              return router.HandleHttpStream(std::move(stream));
            }),
        [&](Status) { ++joined; });
  connectrpc::Client runtime(
      {},
      [connection = *client.value()](connectrpc::Metadata headers,
                                     connectrpc::TransportOptions options) {
        headers.emplace(headers.begin(), ":authority", "localhost");
        headers.emplace(headers.begin(), ":scheme", "http");
        return connection->Open(std::move(headers), options.deadline);
      },
      {[&](const connectrpc::CallInfo& info) {
        return std::make_unique<TraceCall>(info, client_calls);
      }});
  std::make_shared<Conversation>(loop.get_executor(), std::move(runtime),
                                 *client.value(),
                                 [&](Status status) {
                                   done = true;
                                   succeeded = status.ok();
                                   if (!status.ok())
                                     std::cerr << status.message << '\n';
                                 })
      ->Start();
  loop.run();
  return done && succeeded && joined == 2 && client_calls == 4 &&
                 server_calls == 4
             ? 0
             : 1;
}
