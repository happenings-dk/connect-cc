# Implementation progress

The accepted scope remains in PLAN.md; implementation and executed evidence are
mapped in RELEASE_AUDIT.md. Windows/MSVC verification is now running on GitHub
Actions in the private happenings-dk/connect-cc repository. Completion is not
claimed until the required checks pass.

## Present

- Apache-2.0 license, matching connect-rust; local vcpkg and Conan static-runtime
  recipes, dependency locks and independent package consumers. Both managers
  pass the coroutine/callback examples and all 21 public headers. Conan's
  dependency versions also pass the full protocol matrix; see PACKAGING.md.
- Pinned vcpkg dependency baseline, CI workflow, reproducible Linux GCC/Clang
  container builds, independent installed-header consumer, separate sanitizer
  configurations and a bounded protocol parser fuzz target. See DEPENDENCIES.md,
  FUZZING.md and CONFORMANCE.md for executed evidence versus configured jobs.
- Complete-response transport writes keep headers and initial body together,
  preserving queue limits, trailers and flush semantics. Repeated official
  Linux timeout cases pass over both HTTP versions.

- CMake install/export, result/status, protobuf/JSON and bounded gzip codecs.
- Client/server call interceptors with per-call state, asynchronous request
  hooks, ordered metadata hooks, serialized message-size observations and
  exactly-once completion. Authentication work joins on cancellation/deadline;
  success and rejection pass real network tests across all supported protocol
  and shape combinations. See INTERCEPTORS.md.
- Callback completion adapter over the same RPC and transport coroutines,
  preserving associated executors, allocators and terminal cancellation.
  Generated clients exercise all RPC shapes through callbacks. A standalone
  installed-package example demonstrates concurrent stream reads/writes,
  half-close, terminal status and connection shutdown; see CALLBACKS.md.
- Common HTTP connection API with verified TLS negotiation and automatic engine
  selection, h2 preference and permitted HTTP/1.1 fallback. Private socket
  handoff preserves TLS state without a second handshake. Cancellation,
  deadlines, mutual TLS, policy combinations and real RPC handoff have local
  integration coverage; see TLS_NEGOTIATION.md.
- Configurable CORS middleware with explicit origins, credentials, preflight
  handling, custom metadata exposure, cache variation and policy-owned response
  headers. All transport-supported RPC shapes pass TCP/TLS wire tests on both
  HTTP versions, including GET and errors; see CORS.md. No browser-engine or
  separate official CORS certification is claimed.
- Connect GET for side-effect-free unary methods, including generated client
  and server method semantics, deterministic protobuf/JSON serialization,
  bounded URL/base64/percent decoding and independent decompression limits.
  TCP/TLS tests and official profiles cover both HTTP versions; see GET.md.
- llhttp-backed incremental HTTP/1.1 framing and a bounded writer: fixed and
  chunked bodies, trailers, informational responses, close-delimited EOF,
  explicit keep-alive admission and retained pipeline bytes. Header/framing
  ambiguity, forbidden trailers and oversize input are rejected.
- HTTP/1.1 TCP/TLS connections and shared RPC serving with bounded read/write
  queues, sequential keep-alive reuse, partial writes, early responses,
  cancellation, deadlines and bounded shutdown. Connect and gRPC-Web unary,
  server streaming and client streaming pass official profiles; gRPC/bidi
  require HTTP/2. See HTTP1.md.
- Explicit TLS h2/HTTP/1.1 ALPN policies and negotiated-protocol inspection.
  Dual-mode servers prefer h2, and absent ALPN permits HTTP/1.1 only when
  allowed. Existing HTTP/2 transports still require h2. Real loopback tests
  cover all policy pairs, absent ALPN and reversed offer order.
- Incremental protocol envelope framing with bounded allocation.
- nghttp2 engine, multiplexing, bounded bodies/metadata, reset notification,
  GOAWAY handling, outgoing header validation and bounded output batches.
- Asio coroutine h2c and TLS transport with strands, deadlines and cancellation.
- Incremental HTTP/2 streaming engine with manual receive credit, bounded send
  queues, partial writes, half-close, HTTP trailers and explicit stream release.
- Connect streaming headers and bounded pull decoding, per-message gzip,
  compressed/uncompressed terminal records, error details and trailing metadata.
- Public Client::ServerStream, ClientStream and BidiStream methods with
  transport-independent exchange callbacks and retained operation ownership.
- Server streaming registration and routing for all three shapes, with schema
  validation before server-streaming handlers, response headers, terminal status
  flushing, bounded completion waits and unread-request discard.
- Streaming handler deadline cancellation and joining, including interrupted
  request reads that preserve the response for a deadline terminal. Independent
  transport-closure observers cancel idle handlers on peer reset/disconnect and
  are joined before routing returns.
- Single-message cardinality checks for server-streaming requests and
  client-streaming responses, including missing and extra response detection.
- Server response flush completion followed by NO_ERROR request discard, so
  malformed requests and early errors retain the complete terminal response.
- Independent receive/send message limits, including small requests with larger
  responses. Invalid streaming headers retain streaming error framing when the
  codec is known; HTTP-level media/method rejection remains an HTTP error.
- Transport-independent ByteStream interface and typed Connect message handles
  with concurrent read/write, cancellation, and early-response write termination.
- Multiplexed Asio HTTP/2 byte streams over TCP/TLS: reads and writes can overlap,
  slow consumers apply backpressure, deadlines/reset wake waiters, and shutdown
  drains or terminates calls at a deadline. See STREAMING.md.
- Concurrent unary handlers with independent reader/writer tasks, reset and
  disconnect cancellation, and joining of handlers before the connection returns.
  Handler concurrency is capped, including canceled tasks still completing.
- Public ServeHttp2Connection owns connection I/O and bounded handler admission;
  parent cancellation interrupts admission and graceful draining and joins all
  tasks. Server::HandleHttpStream routes unary and streaming on one connection.
- Unary clients use the shared incremental engine. Existing UnaryHttp2/Tls
  conveniences retain their APIs; Client can use its StreamExchange for all
  shapes. Concurrent upload/read preserves early responses without deadlocking
  on flow control, and writers join before return. Client calls retain their
  exchange even when the Client object is destroyed before coroutine startup.
- gRPC headers, timeout units, percent-encoded status, binary typed details and
  HTTP trailers. All four RPC shapes use the same generated APIs and server
  registrations with a protocol option. Native gRPC unary uploads/readers run
  concurrently and join before return. ReadTrailers exposes EOF trailers without
  waiting for a blocked upload. Later trailers override misplaced initial status.
- gRPC-Web binary/text for all four RPC shapes, header negotiation, incremental
  base64 segments, bounded body trailers and compressed terminal records.
  Mixed-case trailer names and trailers-only gateway metadata are preserved;
  malformed data after a terminal record cannot produce success.
- Peer failures observed after a call deadline report deadline_exceeded even
  when a peer reset is dispatched before the timer callback. The conformance
  adapter also preserves cancellation received during DNS/connection setup.
- Server RPC deadlines cancel cooperative handlers and join them before returning.
- TLS chain and DNS/IP verification, SNI, mandatory h2 ALPN, optional mTLS.
  Trust and identity can come from files or in-memory PEM.
- Connect unary POST codecs, timeout/compression negotiation, structured errors,
  metadata/trailers, typed Client/Server APIs and coroutine handlers.
- Go protoc-gen-connect-cpp with source-relative stubs for all four RPC shapes,
  abstract handlers,
  conflict preflight, Buf/protoc integration and compiled generated examples.
- Generated C++ client/server interoperability with connect-go in both directions.
- Pinned official client and server adapters: 970 client and 970 server cases
  pass across Connect unary and streaming HTTP/2 profiles. This includes all
  four RPC shapes, protobuf/JSON, gzip, h2c, TLS and mTLS. See CONFORMANCE.md.
- The gRPC profile additionally passes 1,084 client and 876 server cases over
  HTTP/2 for all RPC shapes, codecs, compression and TLS choices, with no skips.
- The gRPC-Web binary profile passes 1,110 client and 696 server cases with no
  skips. Text mode has local all-shape TCP/TLS tests, not official suite coverage.
- HTTP/1.1 profiles pass 1,658 client and 1,334 server cases, with no skips.
  Across all five profiles, 4,822 client and 3,876 server cases pass.
- 222 CTest entries covering interceptors, callbacks, codecs, CORS, TLS negotiation, fragmented/malformed frames, multiplexing,
  reset isolation, concurrent handler progress and cancellation, oversized
  responses, GOAWAY, loopback transport, TLS identity, RPC deadlines, generated
  code, cross-language interoperability and official unary/streaming conformance.
  Streaming tests cover flow control, TCP/TLS duplex traffic, blocked operations,
  cancellation handoff, shutdown deadlines and asynchronous handle lifetime.
  Typed stream tests cover protobuf/JSON × identity/gzip over TCP/TLS, missing
  terminal records, duplex cancellation, and early response completion.
  Added public-client shape tests, malformed request responses, oversized
  server-streaming requests rejected early, and deadline-bound flush waits.
  Routing tests cover protobuf/JSON × identity/gzip over TCP/TLS, malformed
  single requests rejected before application code, idle peer cancellation,
  deadline interruption of incomplete requests and late handler success.
  Generated streaming clients/handlers compile imported nested request types
  and distinct response types, and run all three streaming shapes over loopback.
  Serving-loop tests exercise cancellation at capacity, before start and during
  graceful drain, plus unary/bidi progress on the same connection.
  Unary stream tests cover large early responses during blocked uploads,
  cancellation joining the upload, and client lifetime before coroutine startup.
  gRPC tests cover timeout units/overflow, malformed and duplicate status,
  rich error code mismatch, percent escapes, binary metadata, fragmented gzip,
  all streaming shapes over TCP/TLS and unary response metadata/status precedence.
  gRPC-Web tests include independent text negotiation, segmented base64 at every
  chunk size, compressed terminal records, invalid trailing data and early
  trailers-only responses while uploads are blocked.

## Immediate next work

Complete Windows/MSVC verification: build, full CTest suite and independent
installed consumers. Hosted verification has started; a running job is not a
passing result. The requirement-by-requirement audit is complete in RELEASE_AUDIT.md;
it distinguishes this missing required gate from additional configured lanes.

## Known limitations

This is not a released or officially verified RPC library. Windows/MSVC
verification remains pending. Client exchanges provide already-opened byte streams; server routing
accepts byte streams and applies RPC deadlines plus bounded completion waits.
TCP connection establishment/DNS is caller-owned.
There is no connection pool, public server accept loop, or graceful TLS close.
Raw HTTP/2 connections and the shared RPC serving loop have bounded draining;
applications own listener shutdown and joining their serving loops.
Cancellation and shutdown depend
on handlers cooperating with cancellation; arbitrary blocking user code cannot
be interrupted.
Cancellation during a partial response write may require a stream reset;
canceled server reads preserve the response direction for a terminal result.
Unary client transport body limits currently bound both directions.
Streaming and server RPC receive and response limits are independent.
TLS defaults rely on OpenSSL trust paths; native OS trust-store
integration is not implemented. The source is now in the private
`happenings-dk/connect-cc` GitHub repository, as authorized by Rasmus. No public
release, registry submission or upstream endorsement is claimed.

## Latest verification

- `cmake --build build` and CTest: 222/222 passed.
- `cmake --build build-asan` and CTest: 222/222 passed with ASan/UBSan, including
  all ten official conformance runs and generated C++/Go interoperability.
- Linux GCC 14.2, Clang 18, ASan/UBSan and separate TSan each pass 222/222.
  TSan uses matching instrumented dependencies, with no report suppressions.
- Conan's pinned dependency graph independently passes 222/222. Its llhttp 9.3
  exposed acceptance of empty Transfer-Encoding fields; the wrapper now rejects
  them explicitly and a fragmented client/server regression passes across both
  dependency baselines. Both package managers pass separate installed examples
  and public-header compilation. Python recipe lint and workflow lint pass.
- Normative gzip review reproduced rejection of valid concatenated members.
  The shared decoder now accepts them under one aggregate output limit. Three
  new tests cover corruption, truncation, empty members, size boundaries and
  protobuf/JSON unary requests and responses. All listed toolchains and both
  package managers pass again. The refreshed ASan/UBSan fuzz run completed
  7,065,302 inputs in 121 seconds without a finding.
- Connect compression preference now honors identity before gzip in an explicit
  ordered list for POST, GET and streaming, retaining request-encoding inference
  when the list is absent. Two new regressions also preserve gRPC's distinct
  capability-list behavior. A failing test was recorded before the fix. All
  listed toolchains and package consumers pass with this change. The latest
  ASan/UBSan fuzz run completed 5,798,617 inputs in 121 seconds without a finding.
- The optional gRPC C++ benchmark completed 90 measured scenarios and 449,640
  RPCs without errors after the compression fixes. All five client/server pairings pass.
  Raw repetitions, latency/throughput summaries, source fingerprints and
  interpretation limits are in BENCHMARKS.md. The harness also passes macOS
  normal/ASan/UBSan and Linux GCC smoke checks. No performance target is imposed.
- Focused callback/generated tests also pass after extending callback streams
  to HTTP/1.1 Connect and binary/text gRPC-Web. HTTP/2 covers all three protocols
  and all stream shapes. Callback tests cover separate executor threads,
  cancellation timing, allocators, move-only results and client lifetime.
- Go generator race tests, go vet, staticcheck and goimports pass after adding
  idempotency propagation and table-driven generator checks. The full CTest runs include generator tests and Go race interop.
  A shared global Go cache had missing entries during an initial run; final
  runs use GOCACHE=$PWD/build/go-cache. Fresh Buf generation matches the compiled
  protoc fixture byte-for-byte. The final Go race, vet, staticcheck, formatting
  and workflow checks pass.
- Five repeated official gRPC cancellation runs passed (110 cases each); three
  ASan/UBSan timeout runs passed (50 cases each) after the race fixes.
- Clang static analysis of src/cors.cc, src/get.cc, src/http1.cc and src/tls.cc passes without findings.
  Manual scoped review checks framing, allocation bounds and TLS policy; it is
  not a complete security or dependency audit. Static analysis of the new
  HTTP/1.1 connection reports two Asio coroutine-internal warnings. The common
  negotiated connection reports one of the same warnings. Both reproduce in
  standalone Asio-only functions; sanitizer/lifecycle tests pass. These are
  analyzer limitations, not a clean static-analysis claim for this transport.
- Deterministic cancellation-at-open regression reproduced a public-client
  handoff bug over TCP/TLS. Unary framed calls and stream opens now cancel
  before launching/returning a handle when cancellation arrived during opening.
  Coverage spans every RPC shape across all three protocols. Three additional
  official gRPC-Web cancellation runs pass (110 cases each).
- Installed current library and rebuilt/ran the separate CMake consumer,
  including CORS policy/middleware, Connect GET, the exported llhttp dependency
  and the common HTTP connection/serving APIs.
- Built and ran the all-shape callback example against both normal and
  ASan/UBSan installed packages. The standard standalone consumer also passes.
- All 21 installed public headers compile independently through the exported
  CMake package, including the shared client/server transport APIs.
- A fresh Release runtime-only build passes with tests, Go generation and the
  conformance harness disabled; private gRPC status code generation remains part
  of the C++ runtime build.
- ASan/UBSan builds now compile hash-pinned GoogleTest 1.18.0 with matching
  instrumentation. A standalone GoogleTest-only program reproduced a prebuilt
  Homebrew archive failure before test discovery; no sanitizer checks were
  disabled. See SANITIZERS.md.
- Linux GCC/Clang pass the full official matrix and installed consumers. A finite
  Linux ASan/UBSan parser fuzz campaign passed. Windows/MSVC is still required
  before release; see CONFORMANCE.md and RELEASE_AUDIT.md.
- Protobuf and connect-go dependencies are pinned in go.mod/go.sum; the official
  conformance revision is pinned in cmake/Conformance.cmake.
- Generated-message ASan initially failed in Protobuf 36.1; a standalone
  Protobuf-only reproducer showed the same crash. Tests use
  `CONNECTCPP_PROTOBUF_GLOBALS_COMPAT=ON`, Protobuf's upstream globals-layout
  opt-out for the prebuilt Homebrew runtime. See CODEGEN.md.

- The unary client now uses the shared HTTP/2 stream engine. The regression
  covering cancellation during TLS handshake and exchange still passes, alongside
  new blocked-upload, early-response and ownership tests over TCP/TLS.
