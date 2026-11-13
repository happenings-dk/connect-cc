#include "connectrpc/tls.h"

#include <openssl/ssl.h>

#include <asio/ip/address.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <cstring>
#include <string_view>

namespace connectrpc {
namespace {
constexpr unsigned char kHttp2Alpn[] = {2, 'h', '2'};
constexpr unsigned char kHttp1Alpn[] = {8,   'h', 't', 't', 'p',
                                        '/', '1', '.', '1'};
constexpr unsigned char kBothAlpn[] = {2,   'h', '2', 8,   'h', 't',
                                       't', 'p', '/', '1', '.', '1'};
std::string_view Offers(TlsAlpn alpn) {
  switch (alpn) {
    case TlsAlpn::kHttp2:
      return {reinterpret_cast<const char*>(kHttp2Alpn), sizeof(kHttp2Alpn)};
    case TlsAlpn::kHttp1:
      return {reinterpret_cast<const char*>(kHttp1Alpn), sizeof(kHttp1Alpn)};
    case TlsAlpn::kHttp2OrHttp1:
      return {reinterpret_cast<const char*>(kBothAlpn), sizeof(kBothAlpn)};
  }
  return {};
}
int SelectAlpn(TlsAlpn allowed, const unsigned char** output,
               unsigned char* output_size, const unsigned char* input,
               unsigned int input_size) {
  const unsigned char* http2 = nullptr;
  const unsigned char* http1 = nullptr;
  // Validate all length-prefixed offers before selecting a protocol. Server
  // preference, not the client's list order, decides dual-mode negotiation.
  for (unsigned int offset = 0; offset < input_size;) {
    unsigned int length = input[offset++];
    if (length == 0 || length > input_size - offset)
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    if (length == 2 && std::memcmp(input + offset, "h2", 2) == 0)
      http2 = input + offset;
    if (length == 8 && std::memcmp(input + offset, "http/1.1", 8) == 0)
      http1 = input + offset;
    offset += length;
  }
  if (http2 && allowed != TlsAlpn::kHttp1) {
    *output = http2;
    *output_size = 2;
    return SSL_TLSEXT_ERR_OK;
  }
  if (http1 && allowed != TlsAlpn::kHttp2) {
    *output = http1;
    *output_size = 8;
    return SSL_TLSEXT_ERR_OK;
  }
  return SSL_TLSEXT_ERR_ALERT_FATAL;
}
Status TlsError(const asio::error_code& error) {
  return {Code::kInvalidArgument, error.message(), {}};
}
}  // namespace
Status ConfigureTlsContext(asio::ssl::context& context, bool server,
                           const TlsOptions& options) {
  bool has_certificate =
      !options.certificate_file.empty() || !options.certificate_pem.empty();
  bool has_key =
      !options.private_key_file.empty() || !options.private_key_pem.empty();
  if ((!options.trust_file.empty() && !options.trust_pem.empty()) ||
      (!options.certificate_file.empty() && !options.certificate_pem.empty()) ||
      (!options.private_key_file.empty() && !options.private_key_pem.empty())) {
    return {Code::kInvalidArgument, "TLS file and PEM options conflict", {}};
  }
  if ((server && !has_certificate) || has_certificate != has_key) {
    return {Code::kInvalidArgument,
            "TLS identity requires certificate and key",
            {}};
  }
  if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) !=
      1) {
    return {Code::kInternal, "cannot set TLS minimum version", {}};
  }
  asio::error_code error;
  if (!options.trust_pem.empty())
    context.add_certificate_authority(asio::buffer(options.trust_pem), error);
  else if (options.trust_file.empty())
    context.set_default_verify_paths(error);
  else
    context.load_verify_file(options.trust_file, error);
  if (error) return TlsError(error);
  if (has_certificate) {
    if (!options.certificate_pem.empty())
      context.use_certificate_chain(asio::buffer(options.certificate_pem),
                                    error);
    else
      context.use_certificate_chain_file(options.certificate_file, error);
    if (error) return TlsError(error);
    if (!options.private_key_pem.empty())
      context.use_private_key(asio::buffer(options.private_key_pem),
                              asio::ssl::context::pem, error);
    else
      context.use_private_key_file(options.private_key_file,
                                   asio::ssl::context::pem, error);
    if (error) return TlsError(error);
    if (SSL_CTX_check_private_key(context.native_handle()) != 1) {
      return {Code::kInvalidArgument, "certificate and key do not match", {}};
    }
  }
  context.set_verify_mode(
      server ? (options.require_client_certificate
                    ? asio::ssl::verify_peer |
                          asio::ssl::verify_fail_if_no_peer_cert
                    : asio::ssl::verify_none)
             : asio::ssl::verify_peer,
      error);
  if (error) return TlsError(error);
  if (Offers(options.alpn).empty())
    return {Code::kInvalidArgument, "invalid TLS ALPN policy", {}};
  if (server) {
    // Use noncapturing callbacks so the context never retains an options
    // pointer whose lifetime could end before a later handshake.
    switch (options.alpn) {
      case TlsAlpn::kHttp2:
        SSL_CTX_set_alpn_select_cb(
            context.native_handle(),
            [](SSL*, const unsigned char** output, unsigned char* size,
               const unsigned char* input, unsigned int length, void*) {
              return SelectAlpn(TlsAlpn::kHttp2, output, size, input, length);
            },
            nullptr);
        break;
      case TlsAlpn::kHttp1:
        SSL_CTX_set_alpn_select_cb(
            context.native_handle(),
            [](SSL*, const unsigned char** output, unsigned char* size,
               const unsigned char* input, unsigned int length, void*) {
              return SelectAlpn(TlsAlpn::kHttp1, output, size, input, length);
            },
            nullptr);
        break;
      case TlsAlpn::kHttp2OrHttp1:
        SSL_CTX_set_alpn_select_cb(
            context.native_handle(),
            [](SSL*, const unsigned char** output, unsigned char* size,
               const unsigned char* input, unsigned int length, void*) {
              return SelectAlpn(TlsAlpn::kHttp2OrHttp1, output, size, input,
                                length);
            },
            nullptr);
        break;
    }
  }

  return {};
}
Status ConfigureTlsClient(asio::ssl::stream<asio::ip::tcp::socket>& stream,
                          const std::string& hostname, TlsAlpn alpn) {
  if (hostname.empty() || hostname.find('\0') != std::string::npos) {
    return {Code::kInvalidArgument, "invalid TLS hostname", {}};
  }
  asio::error_code error;
  stream.set_verify_mode(asio::ssl::verify_peer, error);
  if (error) return TlsError(error);
  stream.set_verify_callback(asio::ssl::host_name_verification(hostname),
                             error);
  if (error) return TlsError(error);
  asio::ip::make_address(hostname, error);
  if (error &&
      SSL_set_tlsext_host_name(stream.native_handle(), hostname.c_str()) != 1) {
    return {Code::kInternal, "cannot set TLS server name", {}};
  }
  std::string_view offers = Offers(alpn);
  if (offers.empty())
    return {Code::kInvalidArgument, "invalid TLS ALPN policy", {}};
  if (SSL_set_alpn_protos(stream.native_handle(),
                          reinterpret_cast<const unsigned char*>(offers.data()),
                          static_cast<unsigned int>(offers.size())) != 0) {
    return {Code::kInternal, "cannot set TLS ALPN", {}};
  }
  return {};
}
Result<TlsProtocol> NegotiatedTlsProtocol(
    asio::ssl::stream<asio::ip::tcp::socket>& stream, TlsAlpn allowed) {
  if (!SSL_is_init_finished(stream.native_handle()))
    return Result<TlsProtocol>(
        Status{Code::kFailedPrecondition, "TLS handshake is not complete", {}});
  if (Offers(allowed).empty())
    return Result<TlsProtocol>(
        Status{Code::kInvalidArgument, "invalid TLS ALPN policy", {}});
  const unsigned char* selected = nullptr;
  unsigned int length = 0;
  SSL_get0_alpn_selected(stream.native_handle(), &selected, &length);
  if (length == 2 && std::memcmp(selected, "h2", 2) == 0 &&
      allowed != TlsAlpn::kHttp1)
    return Result<TlsProtocol>(TlsProtocol::kHttp2);
  if ((length == 0 ||
       (length == 8 && std::memcmp(selected, "http/1.1", 8) == 0)) &&
      allowed != TlsAlpn::kHttp2)
    return Result<TlsProtocol>(TlsProtocol::kHttp1);
  return Result<TlsProtocol>(
      Status{Code::kUnavailable,
             "peer did not negotiate an allowed HTTP protocol",
             {}});
}
Status CheckHttp2Alpn(asio::ssl::stream<asio::ip::tcp::socket>& stream) {
  auto protocol = NegotiatedTlsProtocol(stream, TlsAlpn::kHttp2);
  return protocol.ok() ? Status{} : protocol.status();
}
}  // namespace connectrpc
