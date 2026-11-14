#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_TLS_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_TLS_H_

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <string>

#include "connectrpc/status.h"

namespace connectrpc {
enum class TlsAlpn { kHttp2, kHttp1, kHttp2OrHttp1 };
enum class TlsProtocol { kHttp2, kHttp1 };
enum class TlsTrust { kSystem, kOpenSsl };
struct TlsOptions {
  // Explicit CA files/PEM override system trust and use OpenSSL verification.
  std::string trust_file;
  std::string certificate_file;
  std::string private_key_file;
  bool require_client_certificate = false;
  // In-memory PEM alternatives. A file and PEM for the same setting conflict.
  std::string trust_pem;
  std::string certificate_pem;
  std::string private_key_pem;
  // Servers prefer h2 in dual mode. Existing users remain h2-only.
  TlsAlpn alpn = TlsAlpn::kHttp2;
  // System uses native policy on macOS/Windows, OpenSSL paths on other OSes.
  // Native verification is offline: it uses local intermediates/revocation
  // caches and does not require fresh online revocation evidence.
  TlsTrust trust = TlsTrust::kSystem;
};
// Configure before constructing streams. Server identity is mandatory; client
// identity is optional. TLS 1.2+ only; ALPN policy is explicit.
Status ConfigureTlsContext(asio::ssl::context& context, bool server,
                           const TlsOptions& options);
// Enables chain and DNS/IP identity verification, SNI for DNS, and ALPN.
Status ConfigureTlsClient(asio::ssl::stream<asio::ip::tcp::socket>& stream,
                          const std::string& hostname,
                          TlsAlpn alpn = TlsAlpn::kHttp2);
// Call only after a successful handshake. No ALPN means HTTP/1.1 only when
// the selected policy permits it. Never silently downgrades h2-only calls.
Result<TlsProtocol> NegotiatedTlsProtocol(
    asio::ssl::stream<asio::ip::tcp::socket>& stream, TlsAlpn allowed);
Status CheckHttp2Alpn(asio::ssl::stream<asio::ip::tcp::socket>& stream);
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_TLS_H_
