# gRPC-Web

All four RPC shapes use the same generated clients and registered handlers as
Connect and gRPC. Set `protocol = Protocol::kGrpcWeb` on `CallOptions` or
`StreamCallOptions`. Binary is the default. Set `grpc_web_text = true` to send
base64 request bodies and request text responses. Text mode requires gRPC-Web;
it is rejected for the other protocols.

```cpp
connectrpc::CallOptions options;
options.protocol = connectrpc::Protocol::kGrpcWeb;
options.grpc_web_text = true;
auto response = co_await client.Unary<RequestMessage, ResponseMessage>(
    "/example.Service/Call", request, options);
```

`Client({}, stream_exchange)` and `Server::HandleHttpStream` share the existing
HTTP transport and routing. Protobuf/JSON, per-message gzip, deadlines,
metadata, typed errors, concurrent upload/read and bounded buffering apply to
binary and text mode. No additional generated files are required.

The terminal envelope has flag `0x80` and contains HTTP-style trailer fields.
Writers use lowercase names and CRLF; readers accept mixed-case names and a
missing final CRLF for interoperability. Compressed terminal envelopes are
bounded and require gzip negotiation. Status codes, percent-encoded messages
and binary rich error details share the gRPC codec. Missing, duplicate,
malformed, oversized or non-final terminal records fail the call.

Text decoding is incremental and accepts separately padded base64 segments,
including segment boundaries inside frames. It retains at most three input
bytes between chunks. Invalid alphabet, invalid padding and incomplete final
quartets fail; writers emit padded segments. The message parser still enforces
message, decompressed-message and terminal metadata limits. Response text/binary
mode is decoded from its content type. At the lower protocol layer,
`StreamingRequest::text` and `accept_text` support independent request and
response encodings; the high-level option enables both.

Trailers-only responses can carry status in the initial HTTP headers. Later
body trailers take precedence over misplaced initial status. When a gateway
combines leading and trailing metadata in a single trailers-only header block,
the custom fields remain visible through `ReadHeaders` and are also returned
as trailing metadata. Reading through EOF is required before trusting terminal
status, so a valid terminal followed by invalid base64 or extra frames cannot
succeed.

The pinned official HTTP/2 profile passes 1,110 client and 696 server cases
without exclusions. That profile exercises binary gRPC-Web; text mode is
covered by local parser tests and all-shape TCP/TLS interoperability within the
library. Local tests also cover arbitrary base64 segment/chunk boundaries,
compressed trailers, large messages, early responses during blocked uploads,
trailers-only responses and malformed data after terminal status.

[HTTP/1.1](HTTP1.md) supports unary, server streaming and client streaming in
binary and text modes. Bidirectional streaming requires HTTP/2. The official
HTTP/1.1 profile includes binary gRPC-Web; text mode has local TCP/TLS coverage.
[Browser CORS middleware](CORS.md) handles preflight and metadata exposure.
Browser clients may impose further streaming restrictions: this native C++
implementation does not remove them. See [Conformance](CONFORMANCE.md) for the
exact coverage and [hosted verification](HOSTED_VERIFICATION.md) for platform
results.

Reference: [gRPC-Web protocol](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-WEB.md).
