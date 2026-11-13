# connect-cc: C++ client and server

## Summary

Build a general-purpose C++20 ConnectRPC implementation in the empty connect-cc directory. Deliver the complete v1 in tested stages: client and server, all four RPC shapes, Connect/gRPC/gRPC-Web, HTTP/1.1 and HTTP/2.

Use the Connect protocol specification (https://connectrpc.com/docs/protocol/) and official conformance suite (https://github.com/connectrpc/conformance) as acceptance criteria, with explicit coverage for each supported protocol and transport combination.

Rasmus clarified that the objective is full protocol compliance suitable for
verification. Passing a reduced capability profile is progress, not completion.
The release gate must cover the full promised matrix with no unexplained
failures or skips that conceal missing required capabilities. See CONFORMANCE.md
for the current evidence and outstanding gates.

## Foundation and public API

- Use standalone Asio for executors, sockets, timers, and coroutines; nghttp2 for HTTP/2; llhttp for HTTP/1.1 parsing; OpenSSL for TLS; Google Protobuf for binary/JSON messages; zlib for gzip.
- Expose Asio executors and awaitables directly. Applications own the event loop; the library creates no hidden worker pool. Offer callback completion adapters over the same operations.
- Use namespace connectrpc, CMake package connectcpp, and exported target connectcpp::connectcpp.
- Provide Client, Server, call options/context, metadata, status/error details, and typed stream handles. Return errors as values; RPC and transport failures must not escape as exceptions.
- Unary operations return a typed response with headers and trailers. Streams expose Read, Write, CloseSend, Finish, and cancellation as applicable. EOF and final RPC status remain distinct.
- Permit one read and one write concurrently per stream. Serialize connection state through Asio strands. Bound buffering and make writes await capacity.
- Generate typed clients and abstract service handlers with a Go protoc-gen-connect-cpp plugin. Reuse Google-generated .pb.h messages; emit .connect.h and .connect.cc files beside them. Support protoc and Buf generation.

## Implementation stages

1. Build and contract foundation
   - Establish CMake, dependency pins, formatting/linting, GoogleTest, Go generator tests, and CI.
   - Define ownership, status, metadata, cancellation, and stream lifecycle contracts.
   - Compile representative generated client/server APIs before implementing networking.
2. HTTP/2 unary vertical slice
   - Implement TLS with ALPN, certificate/hostname verification, optional mutual TLS, and cleartext HTTP/2 prior knowledge.
   - Implement Connect unary requests, service registration/routing, protobuf encoding, and error mapping.
   - Run generated C++ clients against Go servers and Go clients against C++ servers.
   - Introduce client-under-test and server-under-test conformance adapters immediately.
3. Complete streaming
   - Add server streaming, bidirectional streaming, then client streaming.
   - Implement incremental envelopes, half-close, terminal status, trailers, deadlines, cancellation, connection loss, and graceful shutdown.
   - Enforce configurable message, decompressed-message, metadata, queue, and concurrency limits.
   - Couple HTTP/2 flow control to application consumption; test slow readers and writers before adding other protocols.
4. Protocol and HTTP completeness
   - Add gRPC and gRPC-Web adapters over shared transport/framing primitives, preserving their distinct terminal-status rules.
   - Add HTTP/1.1 keep-alive, chunked transfer, and TLS negotiation/fallback. Require HTTP/2 for bidirectional streaming and gRPC.
   - Add protobuf JSON, gzip negotiation, Connect GET for eligible methods, and binary/text gRPC-Web support.
   - Add configurable browser CORS handling, including preflight and exposed metadata headers.
   - Reject unsupported combinations explicitly; never silently change RPC semantics.
5. Usability and release
   - Add client/server interceptors for authentication, metadata, and observability. Disable automatic retries by default.
   - Document coroutine and callback examples for all RPC shapes, TLS setup, cancellation, and shutdown.
   - Provide relocatable CMake installation, vcpkg and Conan recipes, and standalone consumer examples.
   - Publish reproducible benchmarks against gRPC C++, reporting results without imposing speculative performance targets.

## Verification and release gates

- Run generator golden tests plus compilation tests covering imported/nested messages, naming collisions, multiple services, and every streaming shape.
- Test fragmented/coalesced envelopes, malformed headers and bodies, missing terminal status, compression failures, oversized payloads, and typed error details.
- Test cancellation/deadline races, simultaneous read/write, half-close, slow consumers, connection reset, GOAWAY, and bounded shutdown.
- Run cross-language interoperability and pinned official client/server conformance tests. Any exclusions must describe unsupported capability explicitly; unexplained failures block v1.
- Build on Linux GCC/Clang, macOS Apple Clang, and Windows MSVC. Run ASan/UBSan and separate TSan jobs where supported; fuzz framing and protocol parsers.
- Verify installed-package consumers independently of the source tree and test callback/coroutine behavioral parity.

## Defaults and boundaries

- C++20 and modern desktop/server toolchains are required. Unreal, consoles, mobile SDK integration, HTTP/3, custom protobuf runtimes, and Bazel packaging are deferred.
- Keep protocol codecs separate from HTTP transports so additional transports do not require regenerating service APIs.
- Follow Google C++ style and the required Go conventions. Use Asio’s coroutine machinery rather than creating custom promises.
- Start with an independently maintained library. Upstream adoption, registry submissions, repository publication, and release publication are separate follow-up actions.
- No stable binary ABI promise for v1; consumers rebuild against the documented dependency versions.
