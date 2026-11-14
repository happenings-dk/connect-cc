# HTTP/1.1

HTTP/1.1 connections implement Connect and gRPC-Web unary, server streaming and
client streaming over TCP or verified TLS. The pinned official profile passes
1,658 client cases and 1,334 server cases, including JSON/protobuf, gzip/identity
and mutual TLS. Eligible Connect GET is supported; see GET.md. Native gRPC
and bidirectional RPCs require HTTP/2. CORS middleware and automatic TLS engine
selection are available; see CORS.md and TLS_NEGOTIATION.md.

## Connection contract

`Http1Connection::Create` accepts an already-connected TCP socket;
`CreateTls` accepts an SSL stream with a configured context. The application
runs and joins `Run` exactly once. `Open` returns a `Http1ByteStream` for a
client request; `Accept` returns one for a server request. The byte stream is
usable with the same `Client` stream exchange and `Server::HandleHttpStream`
router as HTTP/2. `ServeHttp1Connection` owns `Run` and joins its handler before
returning; applications still own the listener and event loop.

Each connection admits one active exchange. Complete both directions and
release the handle before another `Open`; overlapping calls return
`resource_exhausted`. Completed calls can be canceled without closing the
connection. Canceling or dropping an unfinished call closes the connection,
since HTTP/1.1 has no per-stream reset. A queued release is processed before
opening the next request. Servers retain pipelined bytes but do not admit them
until the previous response has flushed and its handle has been released.

One read and one write may run concurrently. State is serialized on an Asio
strand, without worker threads. Incoming/outgoing queues have separate byte
limits; the writer charges in-flight socket bytes until flush. There is one
bounded parser buffer and at most one encoded chunk waiting for queue capacity.
Caller-owned `Write` arguments are not counted as transport queue memory.
`FinishSend` waits for actual socket flush; `ReadTrailers` requires input EOF
without waiting for output, so early responses can interrupt blocked uploads.

Servers can use `SendResponse` for a complete response. It queues headers and
the initial body without an extra executor dispatch, closes the send direction,
and still waits for queue capacity for large bodies. Call `FinishSend` afterward
to await socket flush. The writer batches already-queued fragments into a single
scatter/gather write; it does not wait for future writes or delay explicit
streaming headers. In-flight batches remain charged against the queue limit.
The same complete-response operation is available on HTTP/2 byte streams.

A canceled server read leaves its response writable. Explicit stream cancellation
aborts the connection. After flushing a response, `DiscardRead` drains unread
request bytes without retaining them. The router bounds this cleanup by its
completion deadline. A raw byte-stream caller must set a deadline itself if it
needs a bounded discard. Peer input EOF wakes closure observers but leaves a
complete request's response writable; the RPC router cancels cooperative handlers
on peer closure. Shutdown stops admission, drains the call and closes, with a
deadline to abort unfinished work.

## Framing contract

`Http1Parser` uses llhttp, hidden behind the public header. A server parser reads
requests; a client parser reads responses. `ReadOne` consumes at most the
configured chunk size and returns at most one event: headers, body bytes or
message end with trailers. Callers retain unconsumed input. Drain events,
including with empty input, before asking the socket for more data; zero-byte
body callbacks do not produce events. `EndInput` signals real transport EOF,
completing close-delimited responses and rejecting truncated messages.

Headers use lowercase names and the existing pseudo-header representation:
`:method`, `:path`, `:authority` for requests, and `:status` for responses.
Absolute-form HTTP/HTTPS targets are normalized, with URI authority taking
precedence over Host. Ordinary query values containing URLs remain unchanged.
Duplicate application metadata and empty values are preserved. Connection-local
fields and fields named by `Connection` are removed from application headers.

The parser stops at message end, leaving a pipelined successor untouched.
`NextMessage` explicitly permits another message only after completion on a
persistent connection. The transport must also ensure that the previous send
direction has completed before reusing the connection. Informational responses
are separate messages; the client transport consumes these before returning
final headers. Unrecognized three-digit statuses are preserved for RPC fallback
mapping, rather than becoming invalid-argument errors at the HTTP parser.

Limits cover each header/trailer block, metadata field overhead and raw framing
bytes between body events, including chunk extensions and whitespace. Body
chunks are bounded independently; there is no aggregate body buffer. Malformed
HTTP, duplicate lengths (including identical duplicates), conflicting
length/transfer headers, unsupported transfer codings, invalid targets,
forbidden trailers and truncated bodies fail. Errors are sticky. HTTP upgrades,
CONNECT tunnels and HTTP/1.0 are explicitly unsupported.

`Http1Encoder` handles one outgoing message. It owns connection and transfer
headers. Explicit content lengths are enforced; otherwise bodies use chunked
transfer. GET/OPTIONS default to zero length. Empty writes never terminate a
chunked body; only `End` emits the terminal chunk. HTTP trailers require chunked
transfer and cannot contain framing, routing or authentication fields. Metadata,
including pseudo-headers, is bounded before copying or serializing it. The
caller splits body writes to the configured chunk size.

## TLS policy

`TlsOptions::alpn` configures the server policy. `ConfigureTlsClient` accepts
an explicit `TlsAlpn` argument:

- `kHttp2`: the default; requires negotiated h2.
- `kHttp1`: offers/selects HTTP/1.1.
- `kHttp2OrHttp1`: permits both, with server preference for h2 regardless of the
  client's offer order.

After a successful handshake, `NegotiatedTlsProtocol` returns the selected
protocol or an error. An absent ALPN result is interpreted as HTTP/1.1 only
when the caller's policy permits it. Incompatible offers fail negotiation.
Existing `Http2Connection` and HTTP/2 convenience APIs remain h2-only and never
silently downgrade. Certificate, hostname, SNI, TLS minimum-version and mTLS
checks are unchanged. Applications must configure contexts before creating
streams. `HttpConnection::CreateTls` negotiates and selects the corresponding
engine automatically; its common API retains each engine's lifecycle contract.

## Verification

Local tests exercise all fragment sizes for representative requests, chunked
bodies, compressed-RPC-compatible binary data, trailers, informational and
close-delimited responses, explicit keep-alive admission, invalid framing,
oversized metadata and bounded body chunks. TLS loopback tests cover every
server/client policy pair, absent ALPN and reversed client preference.
Clang's static analyzer checks the codec and TLS implementation; local
manual review covers trust validation, downgrade behavior, allocation limits,
framing ambiguity, hop-by-hop fields and trailer restrictions. This scoped
review is not a full security audit or dependency vulnerability assessment.

The full local CTest suite passes 222 entries normally and with ASan/UBSan,
including all ten official conformance runs. Socket tests exercise fixed and
chunked requests/responses, metadata/trailers, repeated reuse, canceled reads,
second-call cancellation, slow receivers, large early responses, deadlines,
bounded shutdown, informational statuses and truncated/close-delimited bodies.
RPC loopback tests cover all supported shapes with protobuf/JSON, identity/gzip
and gRPC-Web binary/text. Installed-package consumers compile and link the
connection API separately; all 21 public headers compile independently.

Clang static analysis of the connection reports two coroutine-internal Asio
warnings. Minimal Asio-only functions reproduce both; no Connect code is needed.
The codec/TLS analysis is clean, but the connection scan is not reported as clean.
The complete normal and sanitizer lifecycle tests pass.

[Callbacks](CALLBACKS.md) and [interceptors](INTERCEPTORS.md) use the same
HTTP/1.1 transport. Exact platform results and remaining release gates are in
[HOSTED_VERIFICATION.md](HOSTED_VERIFICATION.md); these checks do not constitute
upstream endorsement.

References: [HTTP/1.1 framing, RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html),
[llhttp API](https://github.com/nodejs/llhttp), and
[OpenSSL ALPN API](https://docs.openssl.org/3.0/man3/SSL_CTX_set_alpn_select_cb/).
