#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_MANAGED_TRANSPORT_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_MANAGED_TRANSPORT_H_
#include <asio/any_io_executor.hpp>
#include <chrono>
#include <memory>
#include <string>

#include "connectrpc/http_connection.h"
#include "connectrpc/rpc.h"
namespace connectrpc {
struct ManagedTransportOptions {
  HttpVersion cleartext_version = HttpVersion::kHttp2;
  HttpConnectionOptions connection;
  TlsOptions tls;
  size_t max_connections = 4;
  std::chrono::milliseconds connect_timeout{10000};
  std::chrono::milliseconds idle_timeout{60000};
};
// Owns DNS, connections, TLS context and connection tasks on your executor.
// HTTP/2 multiplexes; HTTP/1 uses exclusive leases. Pool saturation returns
// ResourceExhausted; no unbounded queue and no RPC is retried. A later call may
// create a replacement for a closed connection. Keep the executor running until
// Close completes. Destruction requests cancellation but cannot join tasks.
class ManagedTransport {
 public:
  static Result<std::shared_ptr<ManagedTransport>> Create(
      asio::any_io_executor executor, std::string base_url,
      ManagedTransportOptions options = {});
  ~ManagedTransport();
  ManagedTransport(const ManagedTransport&) = delete;
  ManagedTransport& operator=(const ManagedTransport&) = delete;
  // The transport must outlive the returned Client and its calls.
  Client MakeClient() const;
  asio::awaitable<Result<std::shared_ptr<ByteStream>>> Open(
      Metadata headers, TransportOptions options = {});
  // Stops admission, cancels dials, drains RPCs and joins connection tasks.
  // Deadline expiration aborts connections. Concurrent Close is rejected.
  asio::awaitable<Status> Close(std::chrono::steady_clock::time_point deadline);

 private:
  class Impl;
  explicit ManagedTransport(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};
}  // namespace connectrpc
#endif
