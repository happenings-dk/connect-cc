# Browser CORS

`connectrpc/cors.h` supplies opt-in middleware for Connect and gRPC-Web browser
clients. It wraps `Server::HandleHttpStream` (or another `HttpStreamHandler`) on
either HTTP transport. The same wrapper adds headers to unary, streaming, GET,
and error responses. It does not change generated service APIs.

```cpp
connectrpc::CorsOptions options;
options.allowed_origins = {"https://app.example", "http://localhost:3000"};
options.allowed_headers = {"authorization", "x-request-id"};
options.exposed_headers = {"x-request-id", "trailer-request-id"};
options.allow_credentials = true;  // Only if the application needs credentials.

auto cors = connectrpc::CorsPolicy::Create(std::move(options));
if (!cors.ok()) co_return cors.status();
auto handler = cors.value()->Wrap(
    [&server](std::shared_ptr<connectrpc::ByteStream> stream) {
      return server.HandleHttpStream(std::move(stream));
    });
co_return co_await connectrpc::ServeHttp2Connection(connection, std::move(handler));
// Use ServeHttp1Connection with an HTTP/1.1 connection instead.
```

The wrapper owns policy and handler copies; the server and registered services
must still outlive their calls. Register services before serving.

## Policy

- Origins are exact serialized HTTP(S) origins without trailing slashes. Scheme,
  hostname and port must match. There are no wildcard or suffix patterns. An
  empty list permits no origins. The opaque origin `null` must be listed
  explicitly if an application deliberately permits it.
- Protocol request headers are included automatically. Application headers,
  including `authorization`, must be explicitly allowed. Header-name comparison
  is case insensitive. Invalid tokens, wildcards and control-header overrides
  in configuration are rejected.
- Credentials default to disabled. Enabling them emits
  `access-control-allow-credentials: true` alongside the exact allowed origin.
- gRPC status/detail headers and protocol compression headers are exposed by
  default. Add application response headers to `exposed_headers`. Connect unary
  trailers are HTTP headers with a `trailer-` prefix; expose their full HTTP
  names. Framed stream trailers remain in the response body.
- Preflight caching defaults to 7,200 seconds; set `max_age` to zero to avoid
  persistent preflight caching. Negative values are invalid.

An OPTIONS request with Origin and Access-Control-Request-Method is a preflight.
An allowed origin, GET/POST method and allowed request-header list receive an
empty 204 response. A disallowed or malformed preflight receives an empty 403
without permission headers. Preflights never invoke RPC handlers and do not
check whether a procedure exists. The router still enforces GET eligibility on
actual RPCs. Ordinary OPTIONS requests continue to the wrapped handler.

Actual requests always reach the wrapped handler. CORS is not authentication or
CSRF protection: a rejected browser origin does not prevent an actual request
from executing. Apply application authorization independently. Allowed origins
receive permission headers even on RPC and routing errors; denied, absent and
duplicate origins receive none. Policy-owned Access-Control-* fields replace
any handler-supplied values, including on denied responses.

All actual responses vary on Origin, including those without an Origin request
header. Existing Vary values are preserved. Preflight responses vary on Origin,
Access-Control-Request-Method and Access-Control-Request-Headers. Private network
access permission is not granted by this middleware.

The decorator forwards streaming operations, transport version, deadlines and
cancellation. Only preflights set their own bounded completion deadline
(`completion_timeout`, five seconds by default); they flush the response and
discard unread input using the underlying transport. Metadata limits still
apply to the resulting headers, so budget for configured CORS fields.

## Whole-message integration

Applications using `Server::Handle` directly can call `Preflight(request.headers)`
first and return its response when present. Otherwise, preserve the request
headers, invoke `Handle`, then call `ApplyResponseHeaders(saved_headers,
response.headers)`. Request and response header arguments must not alias. These
helpers expect the lowercase HTTP metadata names used by both library parsers.

## Verification

Five policy tests cover configuration, defaults, authorization opt-in, header
lists, malformed/duplicate fields, cache variation and response-header ownership.
Two network scenarios run on both HTTP versions over TCP and TLS (eight tests):
preflight, GET, denied origins and routing errors; plus all transport-supported
RPC shapes across Connect, gRPC and binary/text gRPC-Web, including terminal
errors and custom response headers/trailers. Native gRPC and bidirectional
streaming require HTTP/2.

These are wire-level C++ tests; they do not claim browser-engine execution or
official CORS certification. The pinned official RPC conformance suite has no
separate CORS profile. Protocol header requirements follow the
[Connect CORS documentation](https://connectrpc.com/docs/cors/).
