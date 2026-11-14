# Managed client connections

`ManagedTransport` connects a base URL to the typed RPC client. It owns DNS
resolution, TCP connections, TLS context, connection tasks and a bounded pool.
Your application supplies and runs the Asio executor. Connection work uses that
executor; Asio's system resolver may use an internal worker for DNS.
The lower-level `HttpConnection` API remains available for applications
that already manage sockets.

```cpp
#include "connectrpc/managed_transport.h"

// Inside a coroutine, with your io_context running.
auto created = connectrpc::ManagedTransport::Create(
    co_await asio::this_coro::executor, "https://api.example.com");
if (!created.ok()) co_return created.status();
auto transport = *created.value();
auto runtime = transport->MakeClient();
connectrpc::generated::greet::v1::GreetServiceClient client(runtime);

greet::v1::GreetRequest request;
request.set_name("World");
auto response = co_await client.Greet(std::move(request));
// Inspect response.status and response.message here.

auto closed = co_await transport->Close(
    std::chrono::steady_clock::now() + std::chrono::seconds(2));
co_return response.status.ok() ? closed : response.status;
```

Keep the transport alive until its client calls finish. `Close` stops admission,
cancels outstanding dials, drains RPCs to the supplied deadline and joins
connection tasks. Destruction requests cancellation; it cannot synchronously
join asynchronous work. Keep the executor running until `Close` returns.
Server listeners and handler lifetimes remain application-owned.

## Connection policy

| Setting | Default | Behavior |
| --- | --- | --- |
| `max_connections` | 4 | Bounds live/retiring connections and outstanding dials |
| `connect_timeout` | 10 seconds | Cancels DNS, TCP and TLS together at the deadline; the call deadline can shorten it |
| `idle_timeout` | 60 seconds | Retires a connection after its last stream lease is released |
| `cleartext_version` | HTTP/2 | Explicit prior knowledge for `http://`; use HTTP/1 for an HTTP/1-only server |
| `connection.alpn` | HTTP/2 or HTTP/1 | HTTPS negotiates once; h2 is preferred |
| `connection.http1`, `.http2` | Engine defaults | Metadata, queue and stream limits |
| `tls` | System trust | Private CA, mTLS and explicit OpenSSL trust options |

HTTP/2 multiplexes calls. Each HTTP/1 connection admits one lease at a time.
Finish and release streams to return their capacity. An operation retains its
lease while suspended, even if the application drops its handle. Saturation
returns `resource_exhausted`; there is no unbounded waiting queue.

Closed connections are replaced for later calls. **No RPC is retried**, including
calls interrupted by connection loss or GOAWAY. A failed call remains failed;
applications decide whether retrying is safe. Retiring connections temporarily
count toward the limit until their tasks exit.

Cancellation of system DNS is cooperative: an OS lookup already in progress
may finish after its deadline. The dial then returns `deadline_exceeded`
without sending the RPC. `Close` joins that completion, so its wall-clock time
can also depend on the OS resolver. TCP/TLS operations and connection draining
are interrupted at their deadlines. A running application executor is required
for timers and cancellation to make progress.

The URL accepts `http://` and `https://`, an ASCII/punycode hostname or bracketed
IPv6 address, an optional numeric port, and an optional path prefix. Credentials,
queries and fragments are rejected. The transport adds `:scheme` and
`:authority`; callers must not override them. A `/prefix` base path is prepended
to generated procedure paths, including Connect GET targets.

## Verification

Loopback integration tests exercise unary and streaming calls, HTTP/2 concurrent
admission, HTTP/1 capacity errors, connection reuse, idle replacement, TLS,
handshake deadlines and shutdown while dialing. The same tests run in the
Lite-only build. The optional [native trust probe](../examples/native-trust)
performs an HTTPS exchange against a public endpoint without changing OS trust
stores; it needs outbound network access.
