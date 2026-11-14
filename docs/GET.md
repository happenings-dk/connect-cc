# Connect GET

Connect unary calls can use GET when their method declares
`option idempotency_level = NO_SIDE_EFFECTS`. POST remains the default.
`IDEMPOTENT` alone does not make a method eligible: it may still have side effects.
The generator passes method semantics to both the runtime client and server
registration; callers cannot turn an unannotated generated method into GET.

```protobuf
rpc Say(SayRequest) returns (SayResponse) {
  option idempotency_level = NO_SIDE_EFFECTS;
}
```

```cpp
connectrpc::CallOptions options;
options.use_get = true;
auto response = co_await generated_client.Say(request, options);
```

For handwritten bindings, supply `MethodOptions{Idempotency::kNoSideEffects}`
as the final argument to both `Client::Unary` and `Server::RegisterUnary`.
Only Connect unary supports this option; gRPC/gRPC-Web requests reject it
before opening the transport. The server returns HTTP 405 with `Allow: POST`
for a GET targeting a registered method that is not side-effect-free. Unknown
procedures still return 404. A rejected GET never invokes its handler.

## Encoding and limits

Requests carry no HTTP body. Their target uses the parameter order
`connect=v1`, optional `base64=1`, optional `compression=gzip`, `encoding`,
then `message`. Protobuf and compressed payloads use unpadded URL-safe base64;
uncompressed JSON uses percent-encoded UTF-8. Reserved bytes use uppercase
percent escapes and spaces use `%20`. Timeout and response compression
negotiation remain HTTP headers. Responses retain normal unary Connect semantics,
including status, leading/trailing metadata, gzip and typed errors.

GET serialization is deterministic: protobuf uses the runtime's deterministic
coded output, while JSON object keys are sorted recursively with array order
preserved. This produces stable cache keys for the same message. It is not a
promise of canonical bytes across protobuf versions, implementations or schema
changes. gzip output is deterministic for the same input and library version.

The client bounds the target using `TransportOptions::max_metadata_bytes`;
the transport also enforces its complete metadata budget. An overlong target
fails with `resource_exhausted`. There is no automatic POST fallback or retry.

The server accepts arbitrary parameter order, unknown query parameters,
percent-encoded names, padded/unpadded URL-safe base64 and form-style `+` spaces.
Only `base64=1` enables base64; other values are ignored as specified. Known
parameters may appear once; duplicates are rejected to avoid ambiguous payloads.
Non-base64 payloads must be valid UTF-8. Invalid percent escapes, invalid base64,
unsupported versions, missing message/encoding and nonempty GET bodies fail.
An empty protobuf message is allowed. Zero-length messages are not decompressed.
Unsupported codecs return HTTP 415; unsupported compression returns
`unimplemented` with the supported encoding list.

Query payload decoding respects the encoded-message limit, and decompression
respects the independent decompressed-message limit. Unknown parameter names
are inspected only far enough to determine that they are unknown, without
allocating storage proportional to their length.

## Evidence

- Pinned official client/server profiles enable GET for HTTP/1.1 and HTTP/2,
  with no skips or known failures. See [CONFORMANCE.md](CONFORMANCE.md).
- Generated bindings compile and execute GET for an annotated method, reject
  unannotated methods before transport, and reject other protocols.
- TCP/TLS loopback tests cover GET with protobuf/JSON and gzip/identity on both
  HTTP versions, response metadata, repeated connection use, and separate wire
  versus decompressed limits.
- Unit tests cover query syntax, URL limits, malformed UTF-8 and base64, unknown
  parameters, padding, deterministic maps, eligibility and HTTP errors.
- GET checks are included in the 236-entry suite and exercised in normal and
  ASan/UBSan builds. Go generator race tests, vet and staticcheck also cover
  eligibility propagation. See [hosted verification](HOSTED_VERIFICATION.md)
  for exact revisions and platform results.

Reference: [Connect Unary-Get-Request specification](https://connectrpc.com/docs/protocol/#unary-get-request).
