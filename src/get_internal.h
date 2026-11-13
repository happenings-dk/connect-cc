#ifndef CONNECTCPP_SRC_GET_INTERNAL_H_
#define CONNECTCPP_SRC_GET_INTERNAL_H_

#include "connectrpc/http2.h"
#include "connectrpc/status.h"

namespace connectrpc::internal_get {
// GET is normalized to the existing POST codec after validating query syntax.
// HTTP media-type rejection remains separate from the RPC error-code mapping.
struct Decoded {
  HttpMessage message;
  Status status;
  int http_status = 0;
};
Decoded Decode(HttpMessage request, size_t max_bytes);
Status Encode(HttpMessage& request, bool compressed, size_t max_url_bytes);
}  // namespace connectrpc::internal_get
#endif  // CONNECTCPP_SRC_GET_INTERNAL_H_
