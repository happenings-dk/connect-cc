# Call interceptors

`connectrpc/interceptor.h` provides per-call hooks shared by runtime and generated
clients, typed service registrations, and callback and coroutine consumers.
Pass a vector of factories to `Server(interceptors)` or to the third argument of
`Client(unary_exchange, stream_exchange, interceptors)`. The default is empty.
Interceptors do not add retries or extra threads.

```cpp
class TraceCall final : public connectrpc::CallInterceptor {
 public:
  explicit TraceCall(const connectrpc::CallInfo& info) : info_(info) {}
  asio::awaitable<connectrpc::Status> OnRequest(
      connectrpc::Metadata& headers) override {
    if (info_.client) headers.emplace_back("trace-id", "example-trace");
    co_return connectrpc::Status{};
  }
  void OnComplete(const connectrpc::Status& status) override {
    // Record info_.procedure, info_.client, and status in your telemetry sink.
  }
 private:
  const connectrpc::CallInfo& info_;
};
connectrpc::Interceptor trace = [](const connectrpc::CallInfo& info) {
  return std::make_unique<TraceCall>(info);
};
connectrpc::Server server({trace});
connectrpc::Client client(unary_exchange, stream_exchange, {trace});
```

The [installed-package callback example](../examples/callbacks/main.cc) includes
this pattern and checks that both endpoints complete all four RPC shapes once.
Its build instructions are in CALLBACKS.md.

## Hook contract

Factories run once per call and may return `nullptr` to opt out of a procedure.
An empty factory is invalid configuration. `CallInfo` describes the procedure,
RPC shape, protocol, codec, endpoint role, GET choice and absolute deadline.
Its reference remains valid until the interceptor is destroyed. Generated
clients require no changes: configure their underlying runtime `Client`.

Request hooks run in registration order. Response-header, response-trailer and
completion hooks run in reverse order, for only the hooks whose `OnRequest`
was entered. A request rejection stops later request hooks and the handler or
client exchange. Headers precede trailers when both hooks run. Each metadata
hook runs at most once, including repeated stream reads/finishes.

`OnRequest(Metadata&)` may await cooperative asynchronous work, such as fetching
or verifying a token. Return a non-OK status to reject the call. The call's
deadline and cancellation interrupt that work, and the library joins it before
reporting completion. A hook that ignores cancellation can delay joining;
blocking work cannot be made interruptible by the RPC runtime. Never keep a
reference to hook metadata after returning.

Client request hooks run before opening the transport. Server hooks run after
routing and protocol validation and before the registered application handler.
For streaming RPCs this is before reading application messages. Connect unary
requests are bounded and decoded at the wire level before these server hooks;
protobuf message decoding still occurs in the typed handler wrapper afterward.
Unknown routes and rejected HTTP/protocol requests do not enter call hooks.
Authentication logic must check duplicate authorization metadata and validate
credentials using the application's policy. CORS policy is a separate HTTP
layer and is not an authentication interceptor.

Metadata contains application fields, preserving duplicates. Request metadata
is validated after each hook: reserved protocol headers, malformed names and
invalid binary metadata produce errors. Server response hooks modify the fields
sent on the wire; normal encoding validation and limits still apply. Client
response hooks modify the metadata delivered to application code. Hooks cannot
change the protocol, procedure, payload, status, or extend a deadline.

`OnMessage(outbound, bytes)` observes serialized, uncompressed message sizes.
It does not expose message contents or measure HTTP/TLS bytes. Outbound events
count encoding/write attempts, not delivery acknowledgments; a later transport
failure can prevent delivery. Inbound events occur after framing validation but
before typed protobuf decoding, so a later decoder failure can still fail the
call. Events for a given message run in registration order.

`OnComplete(status)` runs once for entered hooks. Unary completion accompanies
return of the response. For Connect unary servers it follows response encoding;
it does not attest that the subsequent HTTP flush reached the peer. Streaming
server completion follows response flushing and unread-request cleanup, or a
failure of that cleanup. Client stream completion normally occurs at `Finish`,
after read EOF, and reflects terminal RPC status. Explicit cancellation,
decoder failure, and failed setup can complete earlier. Merely opening a stream
or observing successful EOF does not report success. Dropping the final stream
state reports the received terminal status if EOF was already validated, a
known failure otherwise, or `canceled` for an abandoned call. Always finish or
cancel streams explicitly to release transport resources predictably.

Factories may execute concurrently for different calls. Hooks for one call do
not overlap, but can run on different executor threads; synchronize shared
telemetry/authentication state. A destructor completion can run on the thread
releasing the last call reference. Factories and synchronous hooks must not
block or throw. Request hooks must return failures as status values. An
unexpected asynchronous hook exception is converted to `internal`.

## Verification

Eight `Interceptor.*` CTest entries cover ordering, metadata round trips,
factory opt-out, invalid metadata, request rejection, call ownership, abandoned
calls, and cancellation/deadline joining during authentication. Real loopback
calls cover every supported shape for Connect, gRPC and binary/text gRPC-Web,
on HTTP/2 and permitted HTTP/1.1 combinations, with successful and rejected
requests. Repeated `Finish` and `Cancel` preserve exactly-once completion.
The full official protocol matrix also runs with the default empty chain.
