#include <algorithm>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/redirect_error.hpp>
#include <asio/signal_set.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "connectrpc/callback.h"
#include "connectrpc/http_connection.h"
#include "connectrpc/managed_transport.h"
#include "greet/v1/greet.connect.h"

namespace {
using connectrpc::Async;
using connectrpc::Status;
using Clock = std::chrono::steady_clock;

class Greeter final
    : public connectrpc::generated::greet::v1::GreetServiceHandler {
 public:
  asio::awaitable<connectrpc::Response<greet::v1::GreetResponse>> Greet(
      connectrpc::Request<greet::v1::GreetRequest> request) override {
    connectrpc::Response<greet::v1::GreetResponse> response;
    response.message.emplace().set_greeting("Hello, " + request.message.name());
    co_return response;
  }
};

// All state is confined to one event-loop thread. This object and its handler
// outlive loop.run(), including the accept task and connection shutdown tasks.
class ServerApp {
 public:
  explicit ServerApp(asio::io_context& loop) : acceptor_(loop) {}
  ServerApp(const ServerApp&) = delete;
  ServerApp& operator=(const ServerApp&) = delete;

  bool Start(unsigned short port) {
    auto registered =
        connectrpc::generated::greet::v1::RegisterGreetServiceHandler(router_,
                                                                      greeter_);
    if (!registered.ok()) {
      std::cerr << registered.message << '\n';
      return false;
    }
    asio::error_code error;
    acceptor_.open(asio::ip::tcp::v4(), error);
    if (!error) acceptor_.bind({asio::ip::address_v4::loopback(), port}, error);
    if (!error) acceptor_.listen(64, error);
    if (error) {
      std::cerr << error.message() << '\n';
      return false;
    }
    port_ = acceptor_.local_endpoint(error).port();
    if (error) return false;
    Async(acceptor_.get_executor(), Accept(), [this](Status status) {
      if (!status.ok()) std::cerr << status.message << '\n';
    });
    return true;
  }

  unsigned short port() const { return port_; }

  void Stop() {
    if (std::exchange(stopping_, true)) return;
    asio::error_code ignored;
    acceptor_.close(ignored);
    // Closing the listener stops admission. Shutdown joins each connection's
    // I/O; ServeHttpConnection joins its handlers. Keep the loop running.
    for (const auto& connection : connections_) {
      Async(acceptor_.get_executor(),
            connection->Shutdown(Clock::now() + std::chrono::seconds(2)),
            [connection](Status) {});
    }
  }

 private:
  asio::awaitable<Status> Accept() {
    while (!stopping_) {
      asio::error_code error;
      auto socket = co_await acceptor_.async_accept(
          asio::redirect_error(asio::use_awaitable, error));
      if (error) {
        if (stopping_) co_return Status{};
        co_return Status{connectrpc::Code::kUnavailable, error.message()};
      }
      if (connections_.size() >= 64) continue;
      auto created = connectrpc::HttpConnection::Create(
          std::move(socket), connectrpc::HttpConnection::Role::kServer,
          connectrpc::HttpVersion::kHttp1);
      if (!created.ok()) {
        std::cerr << created.status().message << '\n';
        continue;
      }
      auto connection = *created.value();
      connections_.push_back(connection);
      Async(acceptor_.get_executor(),
            connectrpc::ServeHttpConnection(
                connection,
                [this](std::shared_ptr<connectrpc::ByteStream> stream) {
                  return router_.HandleHttpStream(std::move(stream));
                }),
            [this, connection](Status status) {
              std::erase(connections_, connection);
              if (!status.ok() && !stopping_) {
                std::cerr << status.message << '\n';
              }
            });
    }
    co_return Status{};
  }

  asio::ip::tcp::acceptor acceptor_;
  Greeter greeter_;
  connectrpc::Server router_;
  std::vector<std::shared_ptr<connectrpc::HttpConnection>> connections_;
  unsigned short port_ = 0;
  bool stopping_ = false;
};

asio::awaitable<Status> Call(std::string url, std::string name) {
  connectrpc::ManagedTransportOptions transport_options;
  transport_options.cleartext_version = connectrpc::HttpVersion::kHttp1;
  auto created = connectrpc::ManagedTransport::Create(
      co_await asio::this_coro::executor, std::move(url), transport_options);
  if (!created.ok()) co_return created.status();
  auto transport = *created.value();
  auto runtime = transport->MakeClient();
  connectrpc::generated::greet::v1::GreetServiceClient client(runtime);
  greet::v1::GreetRequest request;
  request.set_name(name);
  connectrpc::CallOptions options;
  options.transport.deadline = Clock::now() + std::chrono::seconds(5);
  auto response = co_await client.Greet(std::move(request), options);
  if (response.status.ok()) {
    if (!response.message || response.message->greeting() != "Hello, " + name) {
      response.status = {connectrpc::Code::kInternal, "unexpected greeting"};
    } else {
      std::cout << response.message->greeting() << '\n';
    }
  }
  auto closed =
      co_await transport->Close(Clock::now() + std::chrono::seconds(2));
  co_return response.status.ok() ? closed : response.status;
}
}  // namespace

int main(int argc, char** argv) {
  const std::string_view command = argc > 1 ? argv[1] : "help";
  if (command != "serve" && command != "call" && command != "self-test") {
    std::cout << "greeter serve [port]\n"
                 "greeter call [url] [name]\n"
                 "greeter self-test\n";
    return command == "help" ? 0 : 1;
  }
  if ((command == "serve" && argc > 3) || (command == "call" && argc > 4) ||
      (command == "self-test" && argc > 2)) {
    std::cerr << "too many arguments\n";
    return 1;
  }
  asio::io_context loop;
  ServerApp server(loop);
  bool succeeded = false;
  if (command == "serve" || command == "self-test") {
    unsigned short port = command == "self-test" ? 0 : 8080;
    if (argc == 3) {
      std::string_view value = argv[2];
      auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != value.data() + value.size()) {
        std::cerr << "port must be an integer from 0 to 65535\n";
        return 1;
      }
    }
    if (!server.Start(port)) return 1;
  }
  asio::signal_set signals(loop, SIGINT, SIGTERM);
  if (command == "serve") {
    signals.async_wait([&](asio::error_code error, int) {
      if (!error) server.Stop();
    });
    std::cout << "Listening on http://127.0.0.1:" << server.port() << std::endl;
    succeeded = true;
  } else {
    const std::string url =
        command == "self-test"
            ? "http://127.0.0.1:" + std::to_string(server.port())
        : argc > 2 ? argv[2]
                   : "http://127.0.0.1:8080";
    Async(loop.get_executor(), Call(url, argc > 3 ? argv[3] : "World"),
          [&](Status status) {
            succeeded = status.ok();
            if (!succeeded) std::cerr << status.message << '\n';
            server.Stop();
          });
  }
  loop.run();
  return succeeded ? 0 : 1;
}
