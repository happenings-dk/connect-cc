#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <chrono>
#include <iostream>

#include "connectrpc/managed_transport.h"

// Optional online probe: tests native system trust without modifying OS stores.
int main(int argc, char** argv) {
  asio::io_context loop;
  auto created = connectrpc::ManagedTransport::Create(
      loop.get_executor(), argc > 1 ? argv[1] : "https://connectrpc.com");
  if (!created.ok()) {
    std::cerr << created.status().message << '\n';
    return 1;
  }
  auto transport = *created.value();
  bool verified = false;
  auto probe = [&]() -> asio::awaitable<void> {
    connectrpc::TransportOptions options;
    options.deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    connectrpc::Metadata headers{{":method", "GET"}, {":path", "/"}};
    auto opened = co_await transport->Open(std::move(headers), options);
    if (opened.ok()) {
      auto stream = *opened.value();
      auto sent = co_await stream->CloseSend({});
      auto received = co_await stream->ReadHeaders();
      verified = sent.ok() && received.ok();
      size_t bytes = 0;
      while (verified) {
        auto next = co_await stream->Read();
        if (!next.ok()) {
          verified = false;
          break;
        }
        if (!*next.value()) break;
        bytes += (**next.value()).size();
        if (bytes > 4 * 1024 * 1024) {
          verified = false;
          break;
        }
      }
      if (!verified) co_await stream->Cancel();
    } else
      std::cerr << opened.status().message << '\n';
    auto closed = co_await transport->Close(std::chrono::steady_clock::now() +
                                            std::chrono::seconds(2));
    if (!closed.ok()) std::cerr << closed.message << '\n';
  };
  asio::co_spawn(loop, probe(), [&](auto exception) {
    if (exception) verified = false;
  });
  loop.run();
  if (verified) std::cout << "System trust and HTTP exchange verified\n";
  return verified ? 0 : 1;
}
