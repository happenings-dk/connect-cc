# Negotiated HTTP connections

`connectrpc/http_connection.h` provides `HttpConnection`, a common API over the
HTTP/1.1 and HTTP/2 engines. `CreateTls` completes one verified TLS handshake,
inspects ALPN, and moves the same TLS stream into the selected engine. The
handshake is not repeated and buffered TLS data is retained. Cleartext `Create`
requires an explicit `HttpVersion`; HTTP/2 uses prior knowledge, not an upgrade
or protocol-sniffing exchange.

## Server

Configure an SSL context before constructing streams. Use the same ALPN policy
in the context and the connection options. The context must outlive all streams.

```cpp
connectrpc::TlsOptions tls;
tls.certificate_file = "server.pem";
tls.private_key_file = "server-key.pem";
tls.alpn = connectrpc::TlsAlpn::kHttp2OrHttp1;
auto configured = connectrpc::ConfigureTlsContext(context, true, tls);
if (!configured.ok()) co_return configured;

// socket is an already-accepted TCP socket; router is a registered Server.
auto connected = co_await connectrpc::HttpConnection::CreateTls(
    asio::ssl::stream<asio::ip::tcp::socket>(std::move(socket), context),
    connectrpc::HttpConnection::Role::kServer, "", {},
    std::chrono::steady_clock::now() + std::chrono::seconds(10));
if (!connected.ok()) co_return connected.status();
co_return co_await connectrpc::ServeHttpConnection(
    *connected.value(),
    [&router](std::shared_ptr<connectrpc::ByteStream> stream) {
      return router.HandleHttpStream(std::move(stream));
    });
```

`ServeHttpConnection` owns and joins the selected engine's `Run` and handler
tasks. Do not start `Run` separately when using it. Its positive `max_handlers`
argument bounds concurrent HTTP/2 handlers; HTTP/1.1 remains sequential. CORS
middleware can wrap the handler as described in CORS.md. External `Shutdown`
drains active calls to its deadline; cancellation closes and joins the serving
operation using the selected engine's existing behavior.

## Client

Configure the client SSL context with trusted roots (and a client identity for
mutual TLS), connect a TCP socket, and call `CreateTls` with the server hostname.
The hostname is checked against the certificate; DNS hostnames also set SNI.
Use [ManagedTransport](MANAGED_TRANSPORT.md) for URL dialing and pooling, or manage DNS and sockets yourself with this lower-level API.

```cpp
connectrpc::HttpConnectionOptions options;
options.alpn = connectrpc::TlsAlpn::kHttp2OrHttp1;
auto connected = co_await connectrpc::HttpConnection::CreateTls(
    asio::ssl::stream<asio::ip::tcp::socket>(std::move(socket), context),
    connectrpc::HttpConnection::Role::kClient, "api.example", options,
    std::chrono::steady_clock::now() + std::chrono::seconds(10));
if (!connected.ok()) co_return connected.status();
auto connection = *connected.value();

connectrpc::StreamExchange exchange =
    [connection](connectrpc::Metadata headers,
                 connectrpc::TransportOptions call) {
      // HTTP/2 pseudo-headers precede ordinary headers. HTTP/1.1 maps authority
      // to Host and omits scheme from its request line.
      headers.emplace(headers.begin(), ":authority", "api.example");
      headers.emplace(headers.begin(), ":scheme", "https");
      return connection->Open(std::move(headers), call.deadline);
    };
connectrpc::Client client({}, std::move(exchange));
```

Before invoking RPCs, spawn `connection->Run()` on the application event loop.
Retain and join that operation during shutdown, as with `Http2Connection` and
`Http1Connection`; the factory starts no hidden tasks or threads. Client RPCs
share the stream exchange. The common `Open`, `Accept`, `Run`, `Shutdown` and
`Cancel` operations retain their engine state even when the outer connection
wrapper is released before operation startup.

## Negotiation and failure behavior

- Dual mode offers both protocols and configured servers prefer h2. An absent
  ALPN selection is accepted as HTTP/1.1 only if the selected policy permits it.
- `http_version()` is available immediately after successful factory completion,
  and each `ByteStream` reports the same version.
- Choose `TlsAlpn::kHttp2` when native gRPC or bidirectional streaming is required.
  HTTP/1.1 fallback does not emulate those capabilities; the RPC layer rejects
  them explicitly. Connect/gRPC-Web unary, server streaming and client streaming
  remain supported over HTTP/1.1.
- There is no retry, reconnect or protocol change after a failed handshake.
  Trust, hostname and ALPN failures close the supplied stream. Passing an
  partially or fully handshaken stream is rejected; the public factory cannot bypass
  client identity verification through an adoption API.
- The optional deadline applies only to negotiation. It never becomes a later
  RPC deadline. Expiry returns `deadline_exceeded`; coroutine cancellation
  returns `canceled` and closes the socket. Pending handshake I/O finishes before
  the factory returns. A canceled timer callback cannot close a handed-off socket.
- Normal Asio caller rules still apply: cancellation of an application's
  `co_spawn` before its first dispatch can produce an Asio exception before the
  library operation starts. Handle that completion or disable automatic
  cancellation exceptions in the parent coroutine when using status results.
- `options.http1` and `options.http2` configure the selected engine's independent
  metadata, body and queue limits. This lower-level API does not pool connections. Finish and release an
  HTTP/1.1 call before opening another on the same connection.

The explicit-version TLS constructors remain available and keep their original
ALPN requirements. Their ordinary path still performs its own verified
handshake; only the private negotiated handoff bypasses a second handshake.

## Trust and shutdown

When no CA file or PEM is supplied, `TlsTrust::kSystem` uses macOS Security
framework policy or Windows certificate-chain/SSL policy. Other platforms use
OpenSSL's default trust paths. `TlsTrust::kOpenSsl` explicitly selects OpenSSL
paths everywhere. Supplying `trust_file` or `trust_pem` replaces system trust
with that explicit OpenSSL CA configuration; it does not augment native roots.
Reconfiguring a context replaces its prior trust store. Configure contexts before
creating streams; do not change a context while it is in use by connections.
Hostname/IP verification, certificate purpose and ALPN remain mandatory.

Native verification uses local stores and cached chain/revocation information.
It does not perform AIA/OCSP network fetches on the handshake executor. Known
revocation is rejected; missing fresh revocation evidence does not itself fail
verification. Servers must provide the necessary intermediate certificates.
Native OS distrust and trust policy are evaluated directly; exporting a root
bundle is not used as a substitute for platform policy.

`Shutdown(deadline)` drains admitted RPCs, joins HTTP I/O, sends TLS
`close_notify`, reads any in-flight TLS application records and waits for the
peer's alert. The deadline covers the whole process. Peer-initiated clean close
uses a one-second response bound. `Cancel` remains an immediate abort; it does
not promise a TLS alert exchange. A successful RPC response is retained even if
later connection shutdown fails. Applications should inspect connection
shutdown status separately.

## Evidence

Local tests cover the complete three-by-three client/server ALPN policy matrix,
with and without server ALPN advertisement; successful paths use mutual TLS and
invoke real RPCs through the selected engine. They verify h2 preference,
HTTP/1.1 fallback, incompatible policy rejection, and native gRPC/bidirectional
restrictions. Additional tests cover expired and stalled handshake deadlines,
preexisting and pending cancellation on both roles, peer socket closure,
untrusted/wrong-host certificates, invalid configuration, second-handshake
rejection, cleartext operation lifetime, and RPCs after the handshake deadline.

The official RPC conformance profiles continue to exercise the explicit HTTP
engines. Automatic negotiation has separate local integration coverage; its
matrix is not being represented as additional official conformance cases.
