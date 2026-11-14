#ifndef CONNECTCPP_SRC_TLS_NATIVE_H_
#define CONNECTCPP_SRC_TLS_NATIVE_H_
#include <openssl/ssl.h>
namespace connectrpc::internal {
// Returns true when the platform verifier has replaced OpenSSL chain building.
bool ConfigureNativeTrust(SSL_CTX* context);
}  // namespace connectrpc::internal
#endif
