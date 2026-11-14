#include "tls_native.h"

#if defined(__APPLE__) || defined(_WIN32)
#include <openssl/x509v3.h>

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <wincrypt.h>
#include <windows.h>
#endif

namespace connectrpc::internal {
namespace {
std::vector<unsigned char> Der(X509* certificate) {
  int size = i2d_X509(certificate, nullptr);
  if (size <= 0) return {};
  std::vector<unsigned char> bytes(static_cast<size_t>(size));
  auto* cursor = bytes.data();
  if (i2d_X509(certificate, &cursor) != size) return {};
  return bytes;
}
#if defined(__APPLE__)
struct ReleaseCf {
  void operator()(const void* value) const {
    if (value) CFRelease(value);
  }
};
template <typename T>
using Cf = std::unique_ptr<std::remove_pointer_t<T>, ReleaseCf>;
bool Verify(X509_STORE_CTX* store, bool server, const std::string& hostname) {
  Cf<CFMutableArrayRef> certificates(
      CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks));
  if (!certificates) return false;
  auto add = [&](X509* certificate) {
    auto bytes = Der(certificate);
    if (bytes.empty()) return false;
    Cf<CFDataRef> data(CFDataCreate(nullptr, bytes.data(),
                                    static_cast<CFIndex>(bytes.size())));
    if (!data) return false;
    Cf<SecCertificateRef> native(
        SecCertificateCreateWithData(nullptr, data.get()));
    if (!native) return false;
    CFArrayAppendValue(certificates.get(), native.get());
    return true;
  };
  if (!add(X509_STORE_CTX_get0_cert(store))) return false;
  auto* chain = X509_STORE_CTX_get0_untrusted(store);
  for (int i = 0; i < sk_X509_num(chain); ++i)
    if (!add(sk_X509_value(chain, i))) return false;
  Cf<CFStringRef> name(CFStringCreateWithCString(nullptr, hostname.c_str(),
                                                 kCFStringEncodingUTF8));
  if (!name) return false;
  Cf<SecPolicyRef> policy(
      SecPolicyCreateSSL(!server, server ? nullptr : name.get()));
  if (!policy) return false;
  SecTrustRef raw = nullptr;
  if (SecTrustCreateWithCertificates(certificates.get(), policy.get(), &raw) !=
      errSecSuccess)
    return false;
  Cf<SecTrustRef> trust(raw);
  // Verification occurs on the handshake executor: never fetch AIA/OCSP here.
  if (SecTrustSetNetworkFetchAllowed(trust.get(), false) != errSecSuccess)
    return false;
  return SecTrustEvaluateWithError(trust.get(), nullptr);
}
#else
struct CloseStore {
  void operator()(void* store) const { CertCloseStore(store, 0); }
};
bool Verify(X509_STORE_CTX* store, bool server, const std::string& hostname) {
  std::unique_ptr<void, CloseStore> certificates(
      CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr));
  if (!certificates) return false;
  auto bytes = Der(X509_STORE_CTX_get0_cert(store));
  if (bytes.empty()) return false;
  std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)>
      leaf(CertCreateCertificateContext(X509_ASN_ENCODING, bytes.data(),
                                        static_cast<DWORD>(bytes.size())),
           CertFreeCertificateContext);
  if (!leaf) return false;
  auto* untrusted = X509_STORE_CTX_get0_untrusted(store);
  for (int i = 0; i < sk_X509_num(untrusted); ++i) {
    bytes = Der(sk_X509_value(untrusted, i));
    if (bytes.empty() ||
        !CertAddEncodedCertificateToStore(
            certificates.get(), X509_ASN_ENCODING, bytes.data(),
            static_cast<DWORD>(bytes.size()), CERT_STORE_ADD_ALWAYS, nullptr))
      return false;
  }
  CERT_CHAIN_PARA parameters{};
  parameters.cbSize = sizeof(parameters);
  LPSTR usage = const_cast<LPSTR>(server ? szOID_PKIX_KP_CLIENT_AUTH
                                         : szOID_PKIX_KP_SERVER_AUTH);
  parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
  parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
  parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &usage;
  PCCERT_CHAIN_CONTEXT raw = nullptr;
  DWORD flags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL |
                CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY |
                CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
  if (!CertGetCertificateChain(nullptr, leaf.get(), nullptr, certificates.get(),
                               &parameters, flags, nullptr, &raw))
    return false;
  std::unique_ptr<const CERT_CHAIN_CONTEXT, decltype(&CertFreeCertificateChain)>
      chain(raw, CertFreeCertificateChain);
  int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                 hostname.c_str(), -1, nullptr, 0);
  if (size <= 0) return false;
  std::vector<wchar_t> name(static_cast<size_t>(size));
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, hostname.c_str(), -1,
                           name.data(), size))
    return false;
  SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
  ssl.cbSize = sizeof(ssl);
  ssl.dwAuthType = server ? AUTHTYPE_CLIENT : AUTHTYPE_SERVER;
  ssl.pwszServerName = server ? nullptr : name.data();
  CERT_CHAIN_POLICY_PARA policy{};
  policy.cbSize = sizeof(policy);
  // Cached positive revocation evidence rejects a peer. Missing cache entries
  // do not make an otherwise trusted certificate unusable while offline.
  policy.dwFlags = CERT_CHAIN_POLICY_IGNORE_ALL_REV_UNKNOWN_FLAGS;
  policy.pvExtraPolicyPara = &ssl;
  CERT_CHAIN_POLICY_STATUS result{};
  result.cbSize = sizeof(result);
  return CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain.get(),
                                          &policy, &result) &&
         result.dwError == 0;
}
#endif
int VerifyNative(X509_STORE_CTX* store, void*) {
  try {
    auto* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(
        store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    if (!ssl) return 0;
    bool server = SSL_is_server(ssl);
    std::string hostname;
    if (!server) {
      auto* parameters = SSL_get0_param(ssl);
      if (const char* name = X509_VERIFY_PARAM_get0_host(parameters, 0))
        hostname = name;
      else {
        char* ip = X509_VERIFY_PARAM_get1_ip_asc(parameters);
        if (ip) {
          hostname = ip;
          OPENSSL_free(ip);
        }
      }
      if (hostname.empty()) return 0;
    }
    bool verified = Verify(store, server, hostname);
    X509_STORE_CTX_set_error(store,
                             verified ? X509_V_OK : X509_V_ERR_CERT_REJECTED);
    return verified ? 1 : 0;
  } catch (...) {
    // Never unwind through OpenSSL's C verification callback.
    X509_STORE_CTX_set_error(store, X509_V_ERR_OUT_OF_MEM);
    return 0;
  }
}
}  // namespace
bool ConfigureNativeTrust(SSL_CTX* context) {
  SSL_CTX_set_cert_verify_callback(context, VerifyNative, nullptr);
  return true;
}
}  // namespace connectrpc::internal
#else
namespace connectrpc::internal {
bool ConfigureNativeTrust(SSL_CTX*) { return false; }
}  // namespace connectrpc::internal
#endif
