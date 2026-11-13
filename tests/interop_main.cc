#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <charconv>
#include <cstdio>
#include <string_view>

#include "interop/v1/service.connect.h"

namespace {
using Text = google::protobuf::StringValue;
using connectrpc::Code;
using connectrpc::Status;
class Echo final : public connectrpc::generated::interop::v1::EchoHandler {
 public:
  asio::awaitable<connectrpc::Response<Text>> Unary(
      connectrpc::Request<Text> request) override {
    connectrpc::Response<Text> response;
    response.headers = request.headers;
    response.trailers = {{"cost", "1"}};
    if (request.message.value() == "error") {
      response.status = {Code::kPermissionDenied, "denied", {}};
      Text detail;
      detail.set_value("reason");
      auto bytes =
          connectrpc::EncodeMessage(detail, connectrpc::Codec::kProtobuf);
      if (!bytes.ok()) {
        response.status = bytes.status();
        co_return response;
      }
      response.status.details.push_back(
          {"google.protobuf.StringValue", std::move(*bytes.value())});
    } else
      response.message = std::move(request.message);
    co_return response;
  }
};
asio::awaitable<Status> RunServer() {
  auto executor = co_await asio::this_coro::executor;
  asio::ip::tcp::acceptor acceptor(executor);
  asio::error_code error;
  acceptor.open(asio::ip::tcp::v4(), error);
  if (error) co_return Status{Code::kUnavailable, error.message(), {}};
  acceptor.bind({asio::ip::address_v4::loopback(), 0}, error);
  if (error) co_return Status{Code::kUnavailable, error.message(), {}};
  acceptor.listen(1, error);
  if (error) co_return Status{Code::kUnavailable, error.message(), {}};
  auto endpoint = acceptor.local_endpoint(error);
  if (error) co_return Status{Code::kUnavailable, error.message(), {}};
  std::printf("%u\n", static_cast<unsigned int>(endpoint.port()));
  std::fflush(stdout);
  auto [accept_error, socket] =
      co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
  if (accept_error)
    co_return Status{Code::kUnavailable, accept_error.message(), {}};
  connectrpc::Server server;
  Echo handler;
  Status status =
      connectrpc::generated::interop::v1::RegisterEchoHandler(server, handler);
  if (!status.ok()) co_return status;
  co_return co_await connectrpc::ServeHttp2(
      std::move(socket),
      [&server](connectrpc::HttpMessage request)
          -> asio::awaitable<connectrpc::HttpMessage> {
        co_return co_await server.Handle(std::move(request));
      });
}
asio::awaitable<Status> RunClient(uint16_t port) {
  auto executor = co_await asio::this_coro::executor;
  connectrpc::Client runtime(
      [executor, port](connectrpc::HttpMessage request,
                       connectrpc::TransportOptions options)
          -> asio::awaitable<connectrpc::Result<connectrpc::HttpMessage>> {
        asio::ip::tcp::socket socket(executor);
        auto [error] = co_await socket.async_connect(
            {asio::ip::address_v4::loopback(), port},
            asio::as_tuple(asio::use_awaitable));
        if (error)
          co_return connectrpc::Result<connectrpc::HttpMessage>(
              Status{Code::kUnavailable, error.message(), {}});
        request.headers.insert(
            request.headers.begin(),
            {{":scheme", "http"},
             {":authority", "127.0.0.1:" + std::to_string(port)}});
        co_return co_await connectrpc::UnaryHttp2(
            std::move(socket), std::move(request.headers),
            std::move(request.body), options);
      });
  connectrpc::generated::interop::v1::EchoClient client(runtime);
  for (connectrpc::Codec codec :
       {connectrpc::Codec::kProtobuf, connectrpc::Codec::kJson}) {
    for (bool compress : {false, true}) {
      for (std::string value :
           {std::string(70000, 'x'), std::string("error")}) {
        Text input;
        input.set_value(value);
        connectrpc::CallOptions options;
        options.codec = codec;
        options.compress = compress;
        options.headers = {{"request-id", "cpp"}};
        options.transport.deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto response = co_await client.Unary(std::move(input), options);
        if (value == "error") {
          if (response.status.code != Code::kPermissionDenied ||
              response.status.message != "denied" ||
              response.status.details.size() != 1) {
            co_return Status{Code::kInternal,
                             "error mismatch: " + response.status.message,
                             {}};
          }
          Text detail;
          Status status =
              connectrpc::DecodeMessage(response.status.details.front().value,
                                        connectrpc::Codec::kProtobuf, detail);
          if (!status.ok() || detail.value() != "reason")
            co_return Status{Code::kInternal, "detail mismatch", {}};
        } else if (!response.status.ok() || !response.message ||
                   response.message->value() != value) {
          co_return Status{Code::kInternal,
                           "response mismatch: " + response.status.message,
                           {}};
        }
        bool has_cost = false;
        for (const auto& [name, entry] : response.trailers)
          if (name == "cost" && entry == "1") has_cost = true;
        if (!has_cost)
          co_return Status{Code::kInternal, "missing response trailer", {}};
      }
    }
  }
  co_return Status{};
}
}  // namespace
int main(int argc, char** argv) {
  if (argc < 2) return 2;
  uint16_t port = 0;
  bool server = std::string_view(argv[1]) == "server";
  if (!server) {
    if (argc != 3 || std::string_view(argv[1]) != "client") return 2;
    unsigned int value = 0;
    std::string_view text(argv[2]);
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() ||
        value == 0 || value > 65535)
      return 2;
    port = static_cast<uint16_t>(value);
  }
  asio::io_context loop;
  int result = 1;
  asio::co_spawn(loop, server ? RunServer() : RunClient(port),
                 [&result](auto exception, Status status) {
                   if (exception) {
                     std::fprintf(stderr, "unexpected coroutine exception\n");
                     return;
                   }
                   if (!status.ok()) {
                     std::fprintf(stderr, "%s\n", status.message.c_str());
                     return;
                   }
                   result = 0;
                 });
  loop.run();
  return result;
}
