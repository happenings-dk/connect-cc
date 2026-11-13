# connect-cc

Work in progress toward a C++20 ConnectRPC client and server. **Experimental; not a complete or released implementation.** See [the accepted plan](docs/PLAN.md).

Licensed under [Apache-2.0](LICENSE), matching
[connect-rust](https://github.com/connectrpc/connect-rust/blob/main/LICENSE).

Implemented so far:

- CMake library and relocatable `connectcpp::connectcpp` export.
- Status/value result types, protobuf and protobuf JSON codecs.
- Incremental Connect, gRPC, and gRPC-Web envelope parsing with per-message
  bounds and sticky errors. Connect terminal payloads have a separate streaming
  decoder; gRPC status travels in HTTP trailers and gRPC-Web in body trailers.
- gzip compression and bounded decompression across concatenated members.
- nghttp2 client/server unary sessions with multiplexing, reset notifications,
  GOAWAY handling, bounded messages, and bounded output batches.
- Asio h2c and TLS unary transport, deadlines and caller cancellation.
- [Incremental HTTP/2 byte streams](docs/STREAMING.md) over TCP/TLS with bounded
  queues, application-driven flow control, half-close, trailers and shutdown.
- Public server/client/bidirectional streaming client methods, with schema
  cardinality checks and early server error preservation.
- Streaming service routing with response metadata, terminal status flushing,
  cooperative handler deadlines, peer-disconnect cancellation and joined tasks.
- Typed Connect message streams with per-message gzip, terminal status/details,
  trailers, cancellation, and bounded pull decoding.
- Concurrent server handlers, stream-reset cancellation and joined completion.
- Shared unary/streaming HTTP/2 serving loop with bounded admission, cancellation
  and joined shutdown; independent request and response size limits.
- Shared unary client transport with concurrent upload/response progress,
  early-response preservation and joined cancellation.
- [gRPC over HTTP/2](docs/GRPC.md) for all four RPC shapes, including JSON,
  gzip, deadlines, HTTP trailers, trailers-only responses and typed errors.
- [gRPC-Web binary/text](docs/GRPC_WEB.md) for all RPC shapes, incremental base64,
  body trailers and trailers-only responses.
- Verified TLS with DNS/IP checks, mandatory h2 ALPN for HTTP/2 and optional mTLS.
- [HTTP/1.1 connections](docs/HTTP1.md) over TCP/TLS: bounded queues, fixed and
  chunked bodies, sequential keep-alive reuse, cancellation, early responses,
  deadlines and joined serving. Connect and gRPC-Web support unary, server
  streaming and client streaming; native gRPC and bidi require HTTP/2.
- [Connect GET](docs/GET.md) for generated side-effect-free unary methods,
  deterministic protobuf/JSON cache keys, gzip queries and bounded decoding.
- Typed unary Client/Server API, Connect POST wire semantics, compression,
  timeout headers, error details, and leading/trailing metadata.

The [official conformance profiles](docs/CONFORMANCE.md) pass 970 Connect client
and 970 Connect server cases, plus 1,084 gRPC client and 876 gRPC server cases.
gRPC-Web adds 1,110 client and 696 server cases. All RPC shapes are covered
over HTTP/2. HTTP/1.1 adds 1,658 client and 1,334 server cases across Connect
and gRPC-Web (8,698 cases total), including Connect GET.
[Browser CORS middleware](docs/CORS.md) has separate local TCP/TLS tests.
Other release gates remain pending; these profiles do not establish full v1 compliance.
See [progress](docs/PROGRESS.md).

[`Async` callback completion](docs/CALLBACKS.md) adapts generated calls, typed
stream operations and connection operations while preserving Asio executor,
allocator and cancellation associations. A runnable installed-package example
in `examples/callbacks` exercises all four RPC shapes.

[Call interceptors](docs/INTERCEPTORS.md) support asynchronous authentication,
metadata changes and per-call observation on clients and servers. Request hooks
respect deadlines and cancellation; stream completion follows final status.

## Build

Dependencies: CMake 3.25+, C++20 compiler, Ninja (optional), Google Protobuf with
its matching C++ runtime, zlib, standalone Asio, OpenSSL 3+, llhttp, pkg-config, nghttp2, and GoogleTest for tests.

The [pinned vcpkg baseline](docs/DEPENDENCIES.md) and `ci/run-linux.sh` provide
reproducible Linux GCC 14/Clang 18 builds. The CI workflow also defines macOS,
Windows, sanitizer and [parser fuzzing](docs/FUZZING.md) jobs; executed results
are recorded separately in [conformance evidence](docs/CONFORMANCE.md).
The [release acceptance audit](docs/RELEASE_AUDIT.md) maps the implementation to
the full plan and identifies the outstanding Windows/MSVC verification gate.

[vcpkg and Conan recipes](docs/PACKAGING.md) package the static runtime and
support independent CMake consumers. The Go generator is built separately from
the same source snapshot.

[Reproducible gRPC C++ benchmarks](docs/BENCHMARKS.md) include all raw runs,
latency percentiles, dependency/source fingerprints and workload limitations.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

On Homebrew installations, add
`-DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/libnghttp2"` if needed.
Sanitizers: configure a separate build with `-DCONNECTCPP_SANITIZE=ON`.
This defaults `CONNECTCPP_FETCH_GTEST=ON`, downloading the hash-pinned
GoogleTest 1.18.0 source and instrumenting it too. See [sanitizer notes](docs/SANITIZERS.md).

```sh
cmake --install build --prefix "$PWD/install"
cmake -S examples/consumer -B build-consumer -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build-consumer
```

The application owns its Asio event loop. `Http2Connection` provides multiplexed
byte streams; `ServeHttp2Connection` owns its server I/O loop and bounded handler
tasks. `Client({}, stream_exchange)` uses the same byte transport for all RPC
shapes. Lower-level session engines remain available for custom I/O integration;
their access must be serialized. These APIs can change before v1.

TLS callers configure an `asio::ssl::context` with `ConfigureTlsContext` before
creating the stream. `UnaryHttp2Tls` enables peer and hostname verification and
negotiates h2; there is no insecure verification bypass. Provide a PEM trust file
or in-memory PEM for private certificate authorities. The tests create ephemeral certificates and
keys in each build directory, never in the source tree.

[`HttpConnection::CreateTls`](docs/TLS_NEGOTIATION.md) negotiates h2 or HTTP/1.1
and hands the verified TLS stream to the selected engine. Its common connection
API preserves HTTP/1.1 sequential reuse and HTTP/2 multiplexing. HTTP/1.1 fallback
still rejects native gRPC and bidirectional RPCs.

`TransportOptions::deadline` is an absolute monotonic deadline including TLS
handshake time. The current transport accepts an already connected socket, so
DNS and TCP connection deadlines remain the caller's responsibility.

The Go [`protoc-gen-connect-cpp`](docs/CODEGEN.md) plugin generates typed clients
and abstract coroutine handlers for all four RPC shapes, with protoc and Buf.
Generated fixtures are compiled and exercised by CTest.
