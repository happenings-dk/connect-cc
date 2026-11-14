# gRPC over HTTP/2

The runtime supports all four gRPC RPC shapes through the same generated clients
and registered handlers used for Connect. Set `protocol = Protocol::kGrpc` on
`CallOptions` for unary calls or `StreamCallOptions` for streaming calls.

```cpp
connectrpc::Client client({}, stream_exchange);
connectrpc::CallOptions unary;
unary.protocol = connectrpc::Protocol::kGrpc;
auto response = co_await client.Unary<RequestMessage, ResponseMessage>(
    "/example.Service/Call", request, unary);

connectrpc::StreamCallOptions streaming;
streaming.protocol = connectrpc::Protocol::kGrpc;
auto opened = co_await client.BidiStream<RequestMessage, ResponseMessage>(
    "/example.Service/Chat", streaming);
```

The stream exchange must open an HTTP/2 byte stream, add `:scheme` and
`:authority`, and apply the call deadline. gRPC unary requires this exchange;
the optional whole-body `UnaryExchange` is only used for Connect unary.
`Server::HandleHttpStream` detects the protocol from content type and routes
both unary and streaming requests. No service regeneration is required to
switch protocols. See GRPC_WEB.md for binary/text gRPC-Web support.

Protobuf and JSON codecs, identity and per-message gzip compression, custom
metadata, duplicate binary metadata, and typed error details are supported.
Timeouts use gRPC units and bounded decimal fields. Status messages preserve
malformed percent escapes while decoding valid ones. Typed errors use a private
protobuf schema wire-compatible with `google.rpc.Status`; applications do not
need a second generated `google.rpc.Status` class.

gRPC terminal status is decoded from HTTP trailers, or initial headers when the
response has neither data nor a later trailer block. A misplaced initial status
cannot override the actual trailers. `ByteStream::ReadTrailers` exposes trailers
after body EOF without waiting for the outgoing request. This lets early
responses finish while an upload is blocked. Unary uploads and reads progress
concurrently, and the upload task is joined before the call returns.

The historical `ConnectMessageStream` class handles all three protocols. Its selected
protocol changes terminal framing; `TypedStream` and generated method signatures
stay the same. Read through EOF, then check `Finish` for final RPC status.
Client-streaming responses remain provisional until successful completion.

The pinned official profile passes 1,084 client and 876 server cases, with no
skip patterns. Local TCP/TLS tests also exercise all streaming shapes, unary
JSON/protobuf and gzip combinations, large bodies, error details and misplaced
status headers with separate leading/trailing metadata. See CONFORMANCE.md for
the tested capability matrix and verification boundaries.

Protocol references: [gRPC HTTP/2 specification](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md)
and [HTTP fallback status mapping](https://github.com/grpc/grpc/blob/master/doc/http-grpc-status-mapping.md).
