# Connect/gRPC/gRPC-Web streams and HTTP/2 transport

The implementation now has typed Connect message streams on incremental,
multiplexed HTTP/2 over TCP and TLS, with generated clients and service handlers
for all streaming shapes. Official client/server conformance profiles cover
all Connect, gRPC and gRPC-Web RPC shapes over HTTP/2; see CONFORMANCE.md for the exact
matrix and GRPC.md for protocol selection and HTTP trailer semantics.

## Public client calls

`Client` accepts a `StreamExchange` alongside its optional unary exchange. The
stream exchange receives HTTP headers and transport options and returns an opened
`ByteStream`; it adds scheme/authority and applies the deadline while opening the
connection/stream. This keeps generated API signatures independent of HTTP/2.
With no unary exchange supplied, `Client({}, stream_exchange)` also routes
`Unary` through that stream exchange. `UnaryByteStream` writes the request while
reading the response, bounds the response, preserves complete early responses,
and joins its writer before returning. `UnaryHttp2` and `UnaryHttp2Tls` use this
same engine on their exclusively owned connections. All client calls capture
their exchange before suspension. Unary `TransportOptions::max_body_bytes`
currently bounds both request and response; streaming has independent limits.

- `ServerStream<Input, Output>(procedure, input, options)` sends the single
  request and half-closes it, then returns a typed response stream.
- `ClientStream<Input, Output>(procedure, options)` returns a typed stream for
  multiple requests and exactly one successful response. Write requests,
  `CloseSend`, read the response and EOF, then check `Finish`.
- `BidiStream<Input, Output>(procedure, options)` returns a typed stream supporting
  concurrent request writes and response reads.

`StreamCallOptions` controls metadata, codec, gzip, response gzip acceptance,
deadline, and message/decompression/metadata limits. `limits` bounds incoming
responses; `send_limits` independently bounds outgoing requests. Streaming call
operations capture their exchange before suspension, so the `Client` object need
not outlive them. An early server error is retained even when a large single
request is interrupted by the server. Automatic retries remain absent.

`StreamingRequest::shape` carries local schema information and is not sent on the
wire. Client entry points and server routing set it from the method schema.
Single-message directions reject extra messages and
missing messages on successful completion; a non-OK final status may have no
response message.

## Service routing and handler lifetime

Register handlers with `RegisterServerStream`, `RegisterClientStream`, or
`RegisterBidiStream`, or use the generated service registration function. Unary
and streaming methods share duplicate-path detection. Registration must finish
before serving concurrent requests; the server and services outlive all calls.

Pass each accepted byte stream to `Server::HandleStream`. It validates the
headers, negotiates codec/compression, applies the registered shape, invokes and
joins the handler, then sends and flushes the final response. It returns transport
status; handler RPC errors travel in the terminal record. Unknown procedures
receive `unimplemented`. Malformed server-streaming requests, including missing,
extra, truncated or undecodable messages, are rejected before handler invocation.

Server-streaming handlers receive a decoded `Request<Input>` and response stream.
Client-streaming and bidi handlers receive a `CallContext` and typed stream.
Client-streaming handlers return `Response<Output>`; the router writes its one
message. Server-streaming and bidi handlers return `StreamCompletion`. Handlers
must join their own child operations before returning, must not retain the stream,
and must leave `End`/`Finish` to routing. See CODEGEN.md for exact signatures.

Request deadlines cancel cooperative handlers, including blocked request reads.
Read interruption preserves the response direction so the router can send a
deadline terminal. An independent transport-closure observer cancels idle handlers
on peer reset or disconnect without consuming input. Both handler and observer
are joined before routing returns. Cancellation during a partial response write
can require a reset because appending a terminal to a partial envelope is invalid.
Handlers doing arbitrary blocking work still cannot be forcibly interrupted.

`StreamServerOptions::completion_timeout` defaults to five seconds and bounds
response flushing after the handler has joined. This permits a deadline terminal
to be sent after the request deadline while bounding waits for peer flow control.
`StreamServerOptions::limits` bounds incoming requests and `send_limits` bounds
outgoing responses, so small request limits do not restrict larger responses.

Use `Server::HandleHttpStream` to route both unary and streaming calls on the
same connection. Unary deadlines include request-body receipt and handler time.
`ServeHttp2Connection(connection, handler, max_handlers)` owns `Run`, accepts
streams, and bounds concurrent handler tasks (100 by default). Do not start
`Run` separately when using this helper. Pass a handler that calls
`server.HandleHttpStream(stream, options)`. Canceled handlers retain their slots
until they join. Connection loss cancels all handlers; parent cancellation
closes the connection and joins handlers and I/O, including during a graceful
drain. Calling `connection->Shutdown(deadline)` stops admission and drains the
serving loop. Applications still own the TCP listener and must join the serving
operation and implement their listener accept loop.

## Connect message streams

`streaming_protocol.h` implements streaming request/response headers, per-message
gzip, and JSON end-stream records. `StreamingDecoder` consumes one envelope at a
time and leaves unconsumed input in the caller's view. It does not collect a
queue of decoded messages. Terminal records may be compressed on input; our
encoder emits uncompressed terminal records. This follows the pinned suite's
compressed-terminal and tolerant error-field cases. Unknown error codes map to
`unknown`; missing/null error codes retain the message and details. A null error
field is treated as absent. Writers always emit a valid, non-null error code.

`StreamLimits` independently bounds wire message size, decompressed message
size, and terminal metadata (including the JSON record). Defaults are 4 MiB,
4 MiB, and 16 KiB. The parser rejects oversized envelope lengths before reading
payloads and bounds decompression, including compressed terminal records.

`ConnectMessageStream` uses the `ByteStream` transport interface, currently
implemented by both `Http2ByteStream` and `Http1ByteStream`. Its client factory takes an already-opened
HTTP request stream and its `StreamingRequest` settings. Encode the headers
with `EncodeStreamingRequest`, add `:scheme` and `:authority` **before regular
headers**, and pass the deadline to `Http2Connection::Open`. The server factory
takes an accepted byte stream after `ReadHeaders` and `DecodeStreamingRequest`,
plus the chosen response headers/compression. The server response codec must
match the request, and gzip must have been accepted by the client.

`TypedStream<Send, Receive>` wraps message encoding/decoding using Google's
Protobuf messages and the negotiated codec. Its handle retains the underlying
stream. Public operations capture their state before the coroutine starts.
All protocol state runs through a strand; one read operation and one write
operation can overlap. A second same-direction operation fails explicitly.

- `ReadHeaders` returns custom leading metadata.
- `Read` returns one typed message or nullopt for HTTP EOF. EOF is not RPC
  success: an error terminal also produces EOF, with its status in `Finish`.
- `Write` serializes one message, frames/compresses it, and awaits queue capacity.
- Client `CloseSend` half-closes the request. Server `End` sends a final status
  and trailers, then half-closes the response. A server can call `SendHeaders`
  explicitly; otherwise the first `Write` or `End` sends them.
  `SendHeaders(metadata)` sets response metadata and rejects changes after send.
- Client `Finish` requires reading through EOF and preserves the final status,
  details, and trailers. It stops any unfinished request writes, so a server
  that responds early cannot leave the client waiting for request window credit.
  Server `Finish` requires `End`, flushes the response, and discards any unread
  request using HTTP/2 NO_ERROR, preserving the response at the client.
- `FailRead` records codec errors; malformed requests stop server reads while
  leaving `End`/`Finish` available to send the error. Malformed responses cancel
  the client stream.
- `Cancel` wakes both directions. Asio cancellation of a server read preserves
  the response direction for a terminal result; canceling a client read or a
  write aborts the call. Malformed frames,
  missing terminal records, and decompression failures cannot become RPC success.

The byte-stream adapter bounds HTTP chunks; the message layer holds at most one
partial envelope plus one unconsumed chunk, and temporarily the decompressed
message. No list of complete messages is prefetched. A large caller-supplied
write and its serialized/compressed representation also occupy bounded
message-sized storage while that operation is pending.

Factory overloads accept separate receive and send `StreamLimits`. The older
single-limit factory overload applies that limit to both directions. A
client-streaming response is provisional until `Finish` confirms success.
Callback completion and HTTP/1.1 are integrated; see CALLBACKS.md and HTTP1.md.
[Hosted verification](HOSTED_VERIFICATION.md) records platform results and
remaining release gates.

## Connection and stream ownership

`Http2Connection::Create` takes ownership of a connected TCP socket.
`CreateTls` takes ownership of an SSL stream; configure its SSL context first
with `ConfigureTlsContext`. Clients verify the supplied hostname and both sides
require h2 ALPN. DNS and TCP connection establishment remain caller-owned.

The application spawns `Run()` on its own Asio event loop and retains/joins that
operation, or uses `ServeHttp2Connection` for server dispatch and joining.
The library starts no threads. `Open()` creates a client stream and
`Accept()` yields a server stream as soon as request headers arrive, before the
request body completes. Incoming server streams and retained stream handles
count against `max_streams`.

All connection and stream operations enter the connection's strand. The public
methods capture connection ownership when called, before their returned
coroutines start. Dropping the last byte-stream handle cancels unfinished work
and posts buffer release onto that strand; keep the event loop running through
cleanup. A connection's `Run()` operation retains its state until I/O finishes.

## Stream lifecycle

- `ReadHeaders()` waits for initial headers and returns them separately from the
  body. Informational HTTP responses do not become application response headers.
- `Read()` returns one body chunk, an error, or successful `nullopt` for body EOF.
- `SendHeaders()` starts a server response.
- `Write()` waits for bounded outbound queue capacity. It may run concurrently
  with one reading operation. A second operation on the same direction returns
  `failed_precondition`.
- `CloseSend()` queues a half-close after all pending output, optionally with
  HTTP trailers. It does not finish the receive direction or guarantee that the
  peer has received the last bytes.
- `FinishSend()` waits for the queued half-close and trailers to finish writing
  to the socket; an nghttp2 serialization callback alone is not completion.
  It can run independently of incoming request data and wakes on deadline,
  cancellation, and connection failure.
- Server `DiscardRead()` is permitted only after `FinishSend`; it releases
  buffered request credit and stops the unfinished request with NO_ERROR.
- After reading through EOF, `Finish()` waits for both HTTP directions to close
  and returns transport status and trailers. It also returns a known terminal
  error immediately. RPC status remains a separate protocol-layer concern.
- `WaitClosed()` observes transport completion independently of reads/writes.
  Canceling this observer does not reset the stream; one observer may wait.
- `ReadTrailers()` returns HTTP trailers after body EOF, independently of the
  outgoing request. gRPC uses it to decode terminal status before stopping any
  unfinished upload.
- `SetDeadline()` and explicit `Cancel()` reset the stream and wake blocked
  reads/writes. Server read-operation cancellation only interrupts that read,
  permitting a terminal response. Other streams on the connection continue.

Keep the handle until `Finish()` completes. Dropping it immediately after
`CloseSend()` can cancel bytes that have not yet reached the peer.

`Shutdown(deadline)` stops new calls and drains existing streams, then closes
the socket. At its deadline it fails remaining calls and closes the connection.
Client GOAWAY is deferred until queued calls finish, so shutdown cannot discard
request headers that have not yet been transmitted. `Run()` joins its writer
before returning. Application RPC handler tasks are outside this raw transport
and must also be joined by their owner or `ServeHttp2Connection`. Applications
own listener shutdown. A public TLS close-notify convenience is not implemented.

## Bounds and flow control

`Http2StreamOptions` independently bounds metadata, live/retained streams,
outbound bytes per stream, the stream receive window, and the connection receive
window. Defaults are 16 KiB metadata, 100 streams, 64 KiB queued output per
stream, a 65,535-byte stream window, and a 1 MiB connection window. Receive
windows must be at least 65,535 bytes because a peer may transmit the HTTP/2
initial window before seeing our SETTINGS.

The engine disables nghttp2 automatic window updates. `Read()` returns receive
credit only when it hands buffered bytes to the application. At the engine
level, callers explicitly use `Consume()`. Releasing/resetting unread streams
returns their remaining connection credit. Small incoming and outgoing chunks
are coalesced so tiny writes cannot create unbounded queue bookkeeping.

These bounds cover transport queues, plus a bounded serialized output batch.
An in-progress `Write(string)` also owns that caller-supplied string; message
size/decompressed-message limits are enforced by the Connect message layer above.

## Verification

Engine tests fragment wire input down to one byte, exercise interleaved traffic
and trailers, exhaust a stream window, verify that an unrelated stream still
progresses, exhaust outbound queue capacity, restore connection credit after
release, enforce metadata/concurrency limits, and handle reset and GOAWAY.

TCP and TLS tests exchange 256 KiB in both directions with an 8 KiB send queue,
prove a slow consumer blocks the writer, distinguish EOF from final completion,
check deadlines and caller cancellation, reject overlapping reads, and bound
shutdown of an unfinished stream. A lifetime regression test drops the last
stream handle before a previously created read coroutine starts.

Typed stream tests exchange four 70 KiB messages in both directions over TCP
and TLS for protobuf/JSON and identity/gzip, retaining final error details and
trailers. They cover missing terminal status, cancellation while both directions
are blocked, cancellation before an operation begins, and an early server error
that stops reading requests. Codec tests cover all split boundaries, coalesced
empty messages, independent limits, compressed terminal records, malformed JSON,
truncation, trailing data, and unnegotiated compression.

Additional tests cover all three public client methods, missing/extra single
responses, malformed request frames and protobuf messages receiving terminal
errors, early rejection of a large server-streaming request, response flush
under backpressure, and a flush blocked by flow control reaching its deadline.
Routing tests cover all three shapes with both codecs and compression choices
over TCP/TLS, response headers, malformed single requests, deadline interruption
of partial requests, canceled handlers that return a late success, and idle
handler cancellation/joining on stream reset or connection loss.
Serving-loop tests cover canceled handlers retaining admission slots, parent
cancellation at capacity and during graceful drain, cancellation before start,
and a blocked unary handler sharing a connection with progressing bidi traffic.
Small receive limits are tested with substantially larger successful responses.
Unary tests also send a large response without consuming a large upload, cancel
an upload blocked on flow control, and destroy the Client before starting a call.
