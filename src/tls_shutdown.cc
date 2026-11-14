#include "tls_shutdown.h"

#include <openssl/err.h>

#include <asio/as_tuple.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <memory>

namespace connectrpc::internal {
namespace {
Status Error() { return {Code::kUnavailable, "TLS close exchange failed", {}}; }
}  // namespace
asio::awaitable<Status> ShutdownTls(
    asio::ssl::stream<asio::ip::tcp::socket>& stream) {
  SSL* ssl = stream.native_handle();
  // Asio's async_shutdown calls SSL_shutdown repeatedly, which treats unread
  // application records as fatal. HTTP/2 control frames can legitimately be
  // in flight after the last RPC. A terminal BIO handoff lets us send the alert
  // once and drain via SSL_read, as required by OpenSSL's shutdown contract.
  // The enclosing connection has joined all Asio SSL operations before here.
  BIO* old_input = SSL_get_rbio(ssl);
  if (BIO_ctrl_wpending(SSL_get_wbio(ssl)) != 0) co_return Error();
  using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
  Bio input(BIO_new(BIO_s_mem()), BIO_free),
      output(BIO_new(BIO_s_mem()), BIO_free);
  if (!input || !output) co_return Error();
  BIO_set_mem_eof_return(input.get(), -1);
  char bytes[16384];
  while (BIO_ctrl_pending(old_input) != 0) {
    int count = BIO_read(old_input, bytes, sizeof(bytes));
    if (count <= 0 || BIO_write(input.get(), bytes, count) != count)
      co_return Error();
  }
  BIO* incoming = input.get();
  BIO* outgoing = output.get();
  SSL_set_bio(ssl, input.release(), output.release());
  bool sent = false;
  for (;;) {
    ERR_clear_error();
    size_t count = 0;
    bool initiating = !sent;
    int result = sent ? SSL_read_ex(ssl, bytes, sizeof(bytes), &count)
                      : SSL_shutdown(ssl);
    int ssl_error = result > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl, result);
    unsigned long detail = ERR_peek_last_error();
    sent = (SSL_get_shutdown(ssl) & SSL_SENT_SHUTDOWN) != 0;
    // Flush alert/key-update bytes before waiting for input. The memory BIO
    // cannot block on writes, so WANT_WRITE indicates a TLS failure.
    while (BIO_ctrl_pending(outgoing) != 0) {
      int size = BIO_read(outgoing, bytes, sizeof(bytes));
      if (size <= 0) co_return Error();
      auto [error, written] = co_await asio::async_write(
          stream.next_layer(), asio::buffer(bytes, static_cast<size_t>(size)),
          asio::as_tuple(asio::use_awaitable));
      (void)written;
      if (error) co_return Status{Code::kUnavailable, error.message(), {}};
    }
    if ((SSL_get_shutdown(ssl) & (SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN)) ==
        (SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN))
      co_return Status{};
    if (ssl_error == SSL_ERROR_NONE ||
        (initiating && result == 0 && sent && ssl_error != SSL_ERROR_SSL))
      continue;
    if (ssl_error != SSL_ERROR_WANT_READ)
      co_return Status{
          Code::kUnavailable,
          "TLS shutdown error " + std::to_string(ssl_error) + " / " +
              std::to_string(detail) + " " +
              (ERR_reason_error_string(detail) ? ERR_reason_error_string(detail)
                                               : "unknown"),
          {}};
    auto [error, received] = co_await stream.next_layer().async_read_some(
        asio::buffer(bytes), asio::as_tuple(asio::use_awaitable));
    if (error) co_return Status{Code::kUnavailable, error.message(), {}};
    if (BIO_write(incoming, bytes, static_cast<int>(received)) !=
        static_cast<int>(received))
      co_return Error();
  }
}
}  // namespace connectrpc::internal
