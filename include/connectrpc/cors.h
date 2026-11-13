#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_CORS_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_CORS_H_

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "connectrpc/byte_stream.h"
#include "connectrpc/http2.h"
#include "connectrpc/status.h"

namespace connectrpc {
struct CorsOptions {
  // Exact, serialized HTTP(S) origins, without a trailing slash. Empty denies
  // all origins. Wildcards are not supported; "null" requires explicit opt-in.
  std::vector<std::string> allowed_origins;
  // Added to the Connect/gRPC-Web protocol headers. Authorization is opt-in.
  std::vector<std::string> allowed_headers;
  // Added to the gRPC status and compression headers. To expose a Connect unary
  // trailer, use its HTTP name, such as "trailer-request-id".
  std::vector<std::string> exposed_headers;
  bool allow_credentials = false;
  std::chrono::seconds max_age{7200};
  // Bounds preflight response flushing and unread-request discard.
  std::chrono::milliseconds completion_timeout{5000};
};

// Immutable, copyable CORS policy. CORS controls browser access to responses;
// it is not authentication. Disallowed actual requests still reach handlers.
class CorsPolicy {
 public:
  static Result<CorsPolicy> Create(CorsOptions options);
  CorsPolicy(const CorsPolicy&) = default;
  CorsPolicy& operator=(const CorsPolicy&) = default;
  CorsPolicy(CorsPolicy&&) = default;
  CorsPolicy& operator=(CorsPolicy&&) = default;

  // Returns a complete 204/403 response for a CORS preflight, or nullopt for an
  // ordinary request. Does not require a registered RPC or invoke a handler.
  std::optional<HttpMessage> Preflight(const Metadata& request_headers) const;
  // Applies policy-owned headers to an actual response (including errors).
  // Preserves existing Vary fields and replaces all Access-Control-* fields.
  // The two arguments must not alias. Metadata names must be lowercase.
  void ApplyResponseHeaders(const Metadata& request_headers,
                            Metadata& response_headers) const;
  // Decorates any HTTP stream handler, including Server::HandleHttpStream.
  // Retains its own policy and handler copies. Actual RPC deadlines, streaming
  // flow control, cancellation, and transport versions remain unchanged.
  HttpStreamHandler Wrap(HttpStreamHandler handler) const;

 private:
  explicit CorsPolicy(CorsOptions options);
  CorsOptions options_;
};
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_CORS_H_
