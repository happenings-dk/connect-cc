#ifndef CONNECTCPP_SRC_TLS_SHUTDOWN_H_
#define CONNECTCPP_SRC_TLS_SHUTDOWN_H_
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>

#include "connectrpc/status.h"
namespace connectrpc::internal {
// Terminal operation only: HTTP reader/writer must be joined, and the caller
// must enforce a deadline by closing the TCP socket. No subsequent Asio SSL I/O
// is permitted (the SSL object remains valid for inspection and destruction).
asio::awaitable<Status> ShutdownTls(
    asio::ssl::stream<asio::ip::tcp::socket>& stream);
}  // namespace connectrpc::internal
#endif
