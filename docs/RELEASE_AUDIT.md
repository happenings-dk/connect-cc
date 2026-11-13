# Release acceptance audit

The accepted objective is [PLAN.md](PLAN.md), including client and server,
all four RPC shapes, the three protocols, HTTP/1.1 and HTTP/2, and the specified
platform/toolchain gates. The runtime and deliverables below have evidence.
**The full objective is not complete: Windows/MSVC execution is still missing.**
Configured Windows CI is not a substitute for a successful Windows build/run.
Upstream adoption and publishing a repository or release remain separate work.

This audit follows the final compression-preference change on 13 November 2026.
The current suite has 222 CTest entries. The unchanged official matrix comprises
8,698 client/server cases; the additional local tests are not represented as
extra official cases. Exact profiles and suite revision are in CONFORMANCE.md.

## Foundation and public contract

| Requirement | Source and inspected evidence | Result |
| --- | --- | --- |
| C++20; standalone Asio, nghttp2, llhttp, OpenSSL, Protobuf and zlib | Root CMake target, dependency manifests/locks; independent installed consumers | Pass |
| Application-owned executors/coroutines; no hidden worker pool or custom coroutine promises | Public Asio awaitables, connection `Run` contract; runtime source has no thread creation or custom promise types | Pass |
| `connectrpc`, CMake package `connectcpp`, exported target `connectcpp::connectcpp` | Public headers, CMake export/config; consumers use only installed package targets | Pass |
| Client, Server, call options/context, metadata, status/details and typed handles | `rpc.h`, `status.h`, `message_stream.h`; generated API compilation and RPC tests | Pass |
| Errors returned as values; unary response includes metadata; EOF separate from final status | Status/Result implementation, typed response and `Read`/`Finish` contracts; malformed/error/cancellation tests | Pass for library RPC/transport failures; caller-owned Asio startup and non-throwing user callbacks are documented in CALLBACKS.md |
| One read and one write concurrently, serialized connection state, bounded writes | Strand-based connection engines; overlap rejection, slow-reader/window and queue-bound tests; separate TSan run | Pass |
| Go generator, `.connect.h`/`.connect.cc`, Google's message classes, abstract handlers | Generator golden tests and CMake-generated fixture compilation | Pass |
| Protoc and Buf workflows | Fresh Buf generation; emitted service header/source match the compiled protoc fixture byte-for-byte | Pass |
| Ownership, cancellation and stream lifecycle contracts | Public header comments, STREAMING.md and CALLBACKS.md; delayed-start/destruction, half-close and joined-cleanup regressions | Pass |

## Protocol and transport implementation

| Requirement | Source and inspected evidence | Result |
| --- | --- | --- |
| Unary Connect routing, serialization and error mapping | `unary.cc`, `rpc.cc`, generated registration; official unary client/server profiles | Pass |
| Client/server/bidirectional streaming | Shared byte/message stream engines; generated and public APIs; official HTTP/2 profiles include half- and full-duplex bidi | Pass |
| Incremental framing, half-close, terminal status, trailers and connection loss | Fragmented/coalesced envelope tests; missing/duplicate/malformed terminal cases; reset and disconnect tests | Pass |
| Deadlines, cancellation and bounded graceful RPC shutdown | Deadline/handler-join, blocked read/write, admission capacity, GOAWAY and bounded-drain tests over TCP/TLS; additional deadline stress evidence in CONFORMANCE.md | Pass |
| Configurable message/decompressed/metadata/queue/concurrency limits | Public limits/options; independent incoming/outgoing limit tests and official receive-limit capability | Pass |
| HTTP/2 flow control follows application consumption | `SlowReaderStopsPeerAtWindowAndBoundsWriterQueue`, slow duplex and response-flush tests | Pass |
| Shared codecs with distinct gRPC and gRPC-Web completion rules | Protocol adapters; gRPC status trailers, gRPC-Web body trailers/text base64 and typed-detail regressions | Pass |
| HTTP/1.1 keep-alive, chunking, early responses and TLS fallback | llhttp wrapper, HTTP/1 connection, common negotiated connection; HTTP/1 official profile and ALPN matrix | Pass |
| Native gRPC and bidi require HTTP/2; unsupported combinations fail explicitly | Client/server transport checks and rejection tests; no fallback retries | Pass |
| TLS peer/hostname/IP verification, ALPN, mTLS and cleartext prior knowledge | `tls.cc`, common TLS factory; trust/host, three-by-three ALPN, missing ALPN, mTLS and stalled/canceled handshake tests | Pass |
| Protobuf binary/JSON, gzip, Connect GET and binary/text gRPC-Web | Codec/GET/framing tests, generated idempotency tests, official binary/JSON/gzip profiles and local text network matrix | Pass |
| Browser preflight and exposed metadata | CORS policy/middleware plus HTTP/1+2 TCP/TLS integration tests | Pass |
| Protocol codecs independent of HTTP transport/generated service APIs | ByteStream/StreamExchange interfaces; generated calls shared across HTTP engines and protocols | Pass |

Normative review checked the Connect POST/GET/streaming rules against their
codec and transport paths: content types/charset, timeout and version fields,
compression inference, metadata/base64, eligible GET encoding, HTTP status and
error fallback, envelope flags, terminal metadata, and unknown error fields.
The gRPC/gRPC-Web review checked content type, timeout representation, trailers,
percent escaping, status details, base64 text segmentation and required final
status. Existing regression families cover these areas. This is not a claim
that testing exhausts every possible input or constitutes maintainer endorsement.

Two gaps found outside the official matrix were reproduced and fixed:

- Valid consecutive gzip members now decode under one aggregate output bound,
  retaining corruption/truncation/trailing-data rejection.
- Connect honors the first supported encoding in its ordered preference list,
  including identity before gzip, for POST, GET and streaming. Omitted lists
  retain request-encoding inference. gRPC/gRPC-Web retain their capability-list
  semantics. Regression cases exercise both compressed and identity requests.

See the [Connect protocol](https://connectrpc.com/docs/protocol/),
[gRPC HTTP/2 protocol](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md)
and [gzip member format](https://www.rfc-editor.org/rfc/rfc1952.html#section-2.2).

## Usability and distribution artifacts

| Requirement | Evidence | Result |
| --- | --- | --- |
| Authentication, metadata and observation interceptors on both sides | Public interceptor API, docs and eight lifecycle/authentication/metadata tests | Pass |
| Automatic retries disabled | Single-exchange client path; no retry policy or hidden reconnect loop | Pass |
| Coroutine and callback examples for all four shapes | Four complete client coroutine flows in CALLBACKS.md; executable callback example includes all handler shapes and concurrent read/write | Pass |
| TLS setup, cancellation and shutdown documentation | TLS_NEGOTIATION.md, CALLBACKS.md, STREAMING.md and HTTP1.md | Pass |
| Relocatable CMake install, vcpkg and Conan recipes | PACKAGE config/export, both native package builds, standard/callback consumers and 21 separately compiled public headers | Pass |
| Reproducible comparison with gRPC C++ | Optional benchmark target/script, five pairings, repeated raw CSV, toolchain/dependency/source hashes and documented limits | Pass; BENCHMARKS.md records the measured snapshot |
| No stable ABI promise | DEPENDENCIES.md and PACKAGING.md require rebuilding against matched dependency/compiler versions | Documented |
| Independent project; no unrequested publication/adoption | Private happenings-dk/connect-cc repository created with explicit user authorization; no registry submission or release publication | Boundary respected |

## Executed checks and missing gate

- macOS arm64 Apple Clang: 222/222 normal and ASan/UBSan.
- Linux x86-64 GCC 14.2 and Clang 18: 222/222 each.
- Linux Clang 18 ASan/UBSan and separately instrumented TSan: 222/222 each.
- Locked Conan dependency stack: 222/222; vcpkg and Conan installed consumers
  and headers pass independently of library source headers.
- Generator golden/compilation tests, imported/nested/optional messages, naming
  collisions, multiple services and every RPC shape pass. Go race tests, vet,
  staticcheck, gofmt, Buf/protoc comparison and C++ formatting checks pass.
- Parser fuzzing runs under ASan/UBSan with bounded inputs/output and retained
  corpus/logs. Latest counts and limitations are recorded in FUZZING.md.
- CI and package/benchmark workflows pass actionlint. Local commands exercise
  the required runtime matrix above; hosted GitHub verification is now running.
- **Windows/MSVC: awaiting result.** The original Windows runner was unreachable.
  Rasmus authorized a private GitHub repository and hosted Windows verification;
  the replacement job has started. Do not declare v1 complete or Windows supported until the
  configured MSVC build, full CTest suite and installed consumers pass.

Hosted Linux arm64 and the pinned macOS dependency stack now also pass all 222
tests and installed consumers. The macOS runner required a test-only thread
portability fix. [HOSTED_VERIFICATION.md](HOSTED_VERIFICATION.md) records the
exact jobs, revisions, original failure and corrected result. Other pending
jobs remain explicitly unverified; results do not imply every possible
platform/architecture combination.

Application-owned connection establishment, listener lifetime and cooperative
handler cancellation follow the accepted executor model. Graceful RPC drain is
bounded and tested; native OS certificate-store integration, TLS close-notify
conveniences, pooling and custom coroutine machinery are not claimed. The
code generator explicitly supports proto2/proto3 with the full runtime; Editions
and lite-runtime schemas fail explicitly. The wire codecs still use Google's
Protobuf implementation. Deferred Unreal/mobile/consoles, HTTP/3, custom protobuf
runtimes and Bazel remain outside the accepted v1 plan.
