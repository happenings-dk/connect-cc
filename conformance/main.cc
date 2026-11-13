#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <cstdio>
#include <string_view>

#include "conformance/adapter.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  if (argc != 2) return 2;
  namespace adapter = connectrpc::conformance_adapter;
  namespace pb = connectrpc::conformance::v1;
  bool server = std::string_view(argv[1]) == "server";
  if (!server && std::string_view(argv[1]) != "client") return 2;
  for (;;) {
    asio::io_context loop;
    int result = 1;
    if (server) {
      pb::ServerCompatRequest request;
      auto read = adapter::ReadFrame(request);
      if (!read.ok() || !*read.value()) return 2;
      asio::co_spawn(loop, adapter::RunServer(std::move(request)),
                     [&result](auto exception, connectrpc::Status status) {
                       if (!exception && status.ok())
                         result = 0;
                       else
                         std::fprintf(stderr, "server adapter: %s\n",
                                      status.message.c_str());
                     });
      loop.run();
      return result;
    }
    pb::ClientCompatRequest request;
    auto read = adapter::ReadFrame(request);
    if (!read.ok()) {
      std::fprintf(stderr, "%s\n", read.status().message.c_str());
      return 2;
    }
    if (!*read.value()) return 0;
    asio::cancellation_signal cancel;
    asio::steady_timer timer(loop);
    if (request.has_cancel() &&
        request.stream_type() == pb::STREAM_TYPE_UNARY) {
      timer.expires_after(
          std::chrono::milliseconds(request.cancel().after_close_send_ms()));
      timer.async_wait([&cancel](asio::error_code error) {
        if (!error) cancel.emit(asio::cancellation_type::all);
      });
    }
    asio::co_spawn(loop, adapter::RunClient(std::move(request)),
                   asio::bind_cancellation_slot(
                       cancel.slot(),
                       [&result, &timer](auto exception,
                                         pb::ClientCompatResponse response) {
                         timer.cancel();
                         if (exception) {
                           std::fprintf(stderr,
                                        "unexpected client adapter failure\n");
                           return;
                         }
                         if (adapter::WriteFrame(response).ok()) result = 0;
                       }));
    loop.run();
    if (result != 0) return result;
  }
}
