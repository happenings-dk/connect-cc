# Callback completion

`connectrpc/callback.h` adds `Async(executor, operation, completion_token)` over
the existing coroutine operations. It uses Asio's coroutine machinery and the
same protocol/transport paths. It creates no event loop or threads and performs
no retries. Generated clients need no separate callback stubs.

```cpp
connectrpc::Async(loop.get_executor(), client.Say(request),
    [](connectrpc::Response<ResponseMessage> response) {
      if (!response.status.ok()) {
        // Handle response.status, including any typed error details.
        return;
      }
      // Consume response.message, response.headers and response.trailers.
    });
loop.run();
```

`operation` is the awaitable returned by a generated client, runtime client,
typed stream or connection. The callback receives its original result type:

| Operation | Completion argument |
| --- | --- |
| Unary call | `Response<Output>` |
| Open a stream | `Result<TypedStream<Input, Output>>` |
| Read a message | `Result<std::optional<Output>>` |
| Read headers | `Result<Metadata>` |
| Write, CloseSend, Cancel, connection Run/Shutdown | `Status` |
| Stream Finish | `StreamCompletion` |
| Negotiated TLS connection factory | `Result<std::shared_ptr<HttpConnection>>` |

The supported result families are `Status`, `Result<T>`, `Response<T>` and
`StreamCompletion`. A raw `awaitable<void>` or whole-message `Server::Handle`
result is not one of these families; use a status-returning operation or wrap
such an application coroutine with an explicit result. Server registrations
retain their coroutine handler API. `ServeHttpConnection` and
`Server::HandleHttpStream` already return `Status` and accept callback completion.

## All four RPC shapes

The [standalone example](../examples/callbacks/main.cc) runs a local HTTP/2 server
and consumes every RPC shape with plain callbacks. Its service handlers provide
the coroutine counterparts. It sends unary requests, reads server streams,
uploads a client stream, and concurrently reads and writes a bidirectional stream.
It waits for both stream directions before checking final status, then closes
the connection and joins the client and server operations.
Client and server interceptors also record each call's final status; the example
checks four completions per endpoint. See INTERCEPTORS.md for hook ownership,
metadata ordering and asynchronous authentication.

Build against the installed package, independently of the source-tree build:

```sh
cmake --install build --prefix "$PWD/install"
cmake -S examples/callbacks -B build-callbacks -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build-callbacks
./build-callbacks/callbacks
```

The same adapter wraps generated methods or the runtime methods:

```cpp
connectrpc::Async(executor,
    client.Unary<Input, Output>("/example.Service/Say", request), on_unary);
connectrpc::Async(executor,
    client.ServerStream<Input, Output>("/example.Service/Expand", request), on_open);
connectrpc::Async(executor,
    client.ClientStream<Input, Output>("/example.Service/Collect"), on_open);
connectrpc::Async(executor,
    client.BidiStream<Input, Output>("/example.Service/Chat"), on_open);
```

Use call options as usual for codecs, compression, metadata, protocol and
deadlines. HTTP/1.1 restrictions are unchanged. After opening a stream, adapt
`Read`, `Write`, `CloseSend`, `ReadHeaders` and `Finish` individually. Allow at
most one read and one write concurrently. Successful read EOF is not RPC
success: call `Finish` and check its status. Client-streaming output remains
provisional until that status is known.

For coroutine consumers, the following are independent bodies of functions
returning `asio::awaitable<connectrpc::Status>`. `Input` and `Output` are generated
protobuf types; `client`, `procedure`, `request`/`requests`, and call options are
supplied by the application. Set deadlines in the options and retain the
application's event loop and connection until these operations join.

Unary calls return the response and both metadata collections together:

```cpp
auto response = co_await client.Unary<Input, Output>(procedure, request, options);
if (!response.status.ok()) co_return response.status;
// Use *response.message, response.headers and response.trailers here.
co_return connectrpc::Status{};
```

Server streaming sends and half-closes its one request during opening:

```cpp
auto opened = co_await client.ServerStream<Input, Output>(
    procedure, request, stream_options);
if (!opened.ok()) co_return opened.status();
auto stream = std::move(*opened.value());
for (;;) {
  auto next = co_await stream.Read();
  if (!next.ok()) co_return next.status();
  if (!*next.value()) break;
  // Process **next.value() before reading the next message.
}
co_return (co_await stream.Finish()).status;
```

For a client-streaming collection method, send the inputs and half-close, then
read its single response. That response is provisional until `Finish` succeeds:

```cpp
auto opened = co_await client.ClientStream<Input, Output>(
    procedure, stream_options);
if (!opened.ok()) co_return opened.status();
auto stream = std::move(*opened.value());
for (const Input& request : requests) {
  auto written = co_await stream.Write(request);
  if (!written.ok()) co_return written;
}
auto closed = co_await stream.CloseSend();
if (!closed.ok()) co_return closed;
std::optional<Output> response;
for (;;) {
  auto next = co_await stream.Read();
  if (!next.ok()) co_return next.status();
  if (!*next.value()) break;
  response = std::move(**next.value());
}
auto finished = co_await stream.Finish();
if (!finished.status.ok()) co_return finished.status;
// Use the response and finished.trailers here; ReadHeaders supplies headers.
co_return connectrpc::Status{};
```

For a bidirectional echo method, a simple lockstep conversation can alternate
writes and reads. It still drains through EOF after half-closing:

```cpp
auto opened = co_await client.BidiStream<Input, Output>(procedure, stream_options);
if (!opened.ok()) co_return opened.status();
auto stream = std::move(*opened.value());
for (const Input& request : requests) {
  auto written = co_await stream.Write(request);
  if (!written.ok()) co_return written;
  auto reply = co_await stream.Read();
  if (!reply.ok()) co_return reply.status();
  if (!*reply.value()) co_return (co_await stream.Finish()).status;
  // Process **reply.value(). This example expects one echo per request.
}
auto closed = co_await stream.CloseSend();
if (!closed.ok()) co_return closed;
for (;;) {
  auto next = co_await stream.Read();
  if (!next.ok()) co_return next.status();
  if (!*next.value()) break;
  // Process any remaining replies.
}
co_return (co_await stream.Finish()).status;
```

For independent producers and consumers, run the read and write loops as
separate Asio child operations, with at most one operation in each direction,
and join both before destroying their state. The runnable callback example
shows this concurrent pattern. An early return above destroys the local stream
handle; keep the transport running so cancellation and connection cleanup can
finish. See STREAMING.md for ownership and bounded shutdown rules.

## Executors, ownership and cancellation

- A plain callback executes on the supplied executor. `asio::bind_executor`
  selects a different completion executor; both event loops must be run.
  Work tracking keeps completion pending until that executor processes it.
  Associated allocators are forwarded as well.
- The callback never executes inline within the initiating call. Move-only
  handlers and results are supported. `asio::deferred` and `asio::use_awaitable`
  are lazy completion tokens; the operation starts when initiated/awaited.
  Constructing the underlying operation can still perform its usual synchronous
  work, such as serializing a typed `Write` argument.
- Retain application state until completion. The runnable example uses a session
  retained only by pending callbacks. A strand is appropriate when multiple
  threads run callbacks that mutate the same application state.
- Bind `asio::cancellation_signal::slot()` to the callback to cancel the
  operation. Keep the signal alive until completion and use a separate slot for
  each concurrent operation. Reusing a slot early replaces its active handler.
- Only terminal cancellation is accepted, matching `co_spawn` and the RPC
  operations; partial/total-only requests do not promise rollback. Emitting
  `asio::cancellation_type::all` includes terminal cancellation.
- Cancellation received before operation startup returns `canceled` without
  starting the underlying coroutine. Cancellation during execution reaches the
  same coroutine cancellation path. A completed result wins later cancellation,
  including an already-returned stream handle; the caller must still finish or
  cancel that handle.
- Schedule cleanup such as `Cancel` without a canceled slot. Otherwise startup
  cancellation can prevent the cleanup operation itself from running.
- RPC failures remain result values. An unexpected coroutine exception is
  converted to `internal` (or `canceled` when cancellation was signaled), without
  exposing an exception argument. User completion callbacks must not throw.

```cpp
asio::cancellation_signal cancellation;
connectrpc::Async(executor, client.Say(request),
    asio::bind_cancellation_slot(cancellation.slot(), on_unary));
// Later, after initiation; serialize access to the cancellation signal itself.
cancellation.emit(asio::cancellation_type::all);
```

This adapter does not remove the C++20/Asio requirement. Callers can use callbacks
without writing coroutine functions, while the implementation still uses them.

## Verification

Focused tests cover non-inline completion, move-only handlers/results, allocator
forwarding, separate executors and threads, cancellation before startup and
during I/O, late cancellation, lazy tokens, unexpected Asio errors, and unary
result/lifetime parity. Generated bindings execute all streaming shapes with
plain callbacks over Connect, gRPC and binary/text gRPC-Web on HTTP/2, plus
HTTP/1.1 server/client streaming over Connect and binary/text gRPC-Web. The standalone
example is built and run against both normal and ASan/UBSan installed packages.
