# Interoperability and official conformance

The current profiles test **Connect, gRPC and gRPC-Web over HTTP/2**, plus
**Connect and gRPC-Web over HTTP/1.1**, with protobuf/JSON, identity/gzip,
cleartext, TLS and mutual TLS. HTTP/2 covers all four RPC shapes; HTTP/1.1
covers unary, server streaming and client streaming. Native gRPC and bidi
require HTTP/2. The pinned suite passes **4,822 client and 3,876 server cases**
(**8,698 total**):

| Profile | Client | Server |
|---|---:|---:|
| Connect unary POST/GET (`unary.yaml`) | 394 | 416 |
| Connect streaming (`streaming.yaml`) | 576 | 554 |
| gRPC, all shapes (`grpc.yaml`) | 1,084 | 876 |
| gRPC-Web binary, all shapes (`grpc_web.yaml`) | 1,110 | 696 |
| HTTP/1.1 Connect and gRPC-Web (`http1.yaml`) | 1,658 | 1,334 |

This is a capability milestone, not the complete v1 release gate in PLAN.md.

The suite is pinned to
[`73e45c4dda9c133fab1538d6d6c6d66c7deed844`](https://github.com/connectrpc/conformance/tree/73e45c4dda9c133fab1538d6d6c6d66c7deed844).
CMake builds the runner and generates C++ messages from the same checkout.
Its Go module lock controls the reference implementation's dependencies.
The adapters speak the suite's four-byte, big-endian, size-delimited protobuf
stdin/stdout protocol. Adapter input and output frames are bounded to 64 MiB.
Only test protocol messages go to stdout; diagnostics go to stderr.

```sh
cmake -S . -B build -DCONNECTCPP_BUILD_CONFORMANCE=ON
cmake --build build
ctest --test-dir build -L conformance --output-on-failure
```

Enabling this option downloads the pinned source and Go dependencies. It is off
by default, so runtime-only builds do not require the harness. Go 1.26 or newer
is needed by the current Go dependencies.

For individual cases:

```sh
build/bin/connectconformance --mode client --conf conformance/unary.yaml \
  --run 'Client Cancellation/**' -- build/connectcpp_conformance client
build/bin/connectconformance --mode server --conf conformance/unary.yaml \
  --run 'Deadline Propagation/**' -- build/connectcpp_conformance server
```

The profiles separate Connect unary, Connect streaming, gRPC and gRPC-Web
coverage, with an additional HTTP/1.1 profile. Both unary-capable Connect
profiles include eligible GET requests. The official
gRPC-Web profile uses binary framing; local tests cover text mode over TCP/TLS
for all shapes, codecs and compression choices, plus fragmented base64.
There are no skip patterns, known-failing patterns or unexplained failures.
Add capabilities as the runtime and adapters implement them; passing these
profiles must not be described as full conformance. CTest runs client and server
modes for each profile. Use `conformance/streaming.yaml` or `conformance/grpc.yaml`
or `conformance/grpc_web.yaml` or `conformance/http1.yaml` in the commands above to select those cases.

The CTest `Interop.Go` entry separately compiles the Go-generated C++ service
bindings and checks a C++ client against connect-go v1.21.1, and a connect-go
client against the generated C++ server. Both codecs and compression choices
are tested, including messages larger than an HTTP/2 window, metadata, trailers,
and typed errors. The official suite currently uses connect-go v2 through its
own pinned module.

## Latest local verification

- 222/222 CTest entries passed, including all ten official conformance runs.
- The same 222/222 passed with ASan/UBSan. With the installed Homebrew Protobuf
  36.1 runtime, this uses `CONNECTCPP_PROTOBUF_GLOBALS_COMPAT=ON` as explained
  in CODEGEN.md.
- Sanitizer builds use the instrumented, hash-pinned GoogleTest source; see
  SANITIZERS.md for the independently reproduced prebuilt-library mismatch.
- Go race tests, `go vet`, and `staticcheck` passed.
- Installed-package consumer rebuilt and ran independently of source headers;
  all 21 installed public headers compile separately. The runnable all-shape
  callback example, including client/server completion interceptors, also passes
  against normal and ASan/UBSan installed packages.
- Final CTest runs used `GOCACHE=$PWD/build/go-cache` to isolate the Go cache
  after missing entries in the shared global cache interrupted an initial run.
- Regression runs exposed and fixed cancellation before adapter child startup,
  and peer-reset/timer dispatch ordering after expiry. Five repeated cancellation
  runs pass (110 cases each), plus three ASan/UBSan timeout runs (50 cases each).
  Reproduce these subsets with `--run 'Client Cancellation/**'` and
  `--run 'Timeouts/**'` against the gRPC profile.

HTTP/1.1 has codec, TCP/TLS transport, shared RPC routing and official profiles.
Local regressions cover sequential reuse, blocked uploads, early responses,
second-call cancellation, interrupted server reads, deadlines, bounded shutdown,
informational responses and truncated/close-delimited bodies. Official cases
exposed and fixed unrecognized HTTP status preservation and unknown-route shape
inference. No failures were excluded. A deterministic
regression also verifies cancellation immediately after transport open for all
RPC shapes and protocols, over TCP/TLS. Three repeated official gRPC-Web client
cancellation runs pass (110 cases each) after that fix.

The streaming transport, message layer and service routing have engine, protocol,
and TCP/TLS lifecycle tests. Generated streaming bindings run real loopback calls.
Streaming adapters exercise the public client APIs and the shared unary/streaming
server router against the suite's Go reference implementations. Client-streaming
responses are only reported as payloads after successful final status.

GET has generated eligibility checks and deterministic protobuf/JSON encoding.
Local TCP/TLS tests additionally cover gzip queries, malformed URLs, UTF-8 and
base64, unknown parameters, empty messages, separate wire/decompressed limits,
HTTP errors and handler non-invocation. See GET.md.

CORS adds five policy tests and eight real TCP/TLS network tests across both HTTP
versions. They cover preflight, GET, errors, credentials, cache variation,
metadata exposure and all transport-supported RPC shapes. These are local wire
tests, not browser-engine tests or a separate official CORS profile. See CORS.md.

Six additional test entries exercise automatic TLS engine selection, mutual
TLS, the ALPN policy matrix, certificate verification, handshake cancellation
and deadlines, invalid/previously-started handshakes, and operation lifetime.
Real RPCs use the negotiated engines; these local tests are separate from the
official profiles. See TLS_NEGOTIATION.md.

Callback completion has eight focused tests plus a generated-stream integration
test spanning both HTTP versions and every supported protocol/stream shape.
These and the other generated-client tests pass normally and with ASan/UBSan
after the HTTP/1.1 callback coverage expansion. See CALLBACKS.md.

The macOS results use Apple Clang. Linux x86-64 GCC 14.2 and Clang 18
also pass 222/222 tests, including all ten official profiles, against the pinned vcpkg dependencies,
plus installed coroutine/callback consumers and all 21 public headers.
The Linux Clang 18 ASan/UBSan build independently passes all 222 tests.
The separate Linux TSan build also passes 222/222, including all ten official
profiles, with matching instrumented target dependencies and no suppressions.
See SANITIZERS.md for the dependency-layout and container-startup fixes.
Installed coroutine/callback consumers and all 21 public headers also pass
against both Linux ASan/UBSan and TSan packages, independently of source headers.
The Conan dependency stack independently passes 222/222, including all ten
official profiles. vcpkg and Conan packages pass standalone coroutine/callback
consumers and all public headers. Their dependency versions and exact recipe
locks are described in PACKAGING.md. The extra HTTP/1 test rejects empty
Transfer-Encoding fields even with llhttp 9.3, where the original wrapper
accepted them; both fragmented request and response parsing are covered.
Windows/MSVC remains the missing required platform gate. Additional configured
CI lanes are not claimed as executed; see RELEASE_AUDIT.md for the full mapping.
A configured GitHub workflow is not evidence that those hosted jobs have run.

Linux deadline stress exposed a response-header/body scheduling gap at the
client deadline. Complete unary responses now queue headers and initial body
together; HTTP/1.1 batches already-queued fragments without relaxing its byte
limit. Four TCP/TLS tests check empty/small/large bodies, trailers and write
backpressure. `ci/stress-timeouts.sh build-linux-clang 20` passes 20 repetitions
of the official unary and HTTP/1.1 timeout subsets: 1,120 cases, with no
exclusions. These extra repetitions are separate from the 8,698-case matrix.

A Linux Clang 18 ASan/UBSan parser fuzz campaign completed 7,466,962 inputs in
121 seconds without a finding. This finite campaign does not establish
exhaustive coverage; see FUZZING.md for its targets, limits and reproduction.
After the gzip-member fix below, a fresh campaign completed another 7,065,302
inputs in 121 seconds without a finding, using the evolved corpus plus the new
concatenated-member seed.

Normative review also found that the original gzip decoder rejected consecutive
members, although [RFC 1952 section 2.2](https://www.rfc-editor.org/rfc/rfc1952.html#section-2.2)
permits them in one gzip representation. The shared decoder now resets the
inflater between members while retaining one aggregate decompressed-size limit.
Regressions cover empty members, output-buffer boundaries, aggregate limits,
truncated later members, corrupt checksums and trailing garbage, plus protobuf
and JSON unary requests/responses. A concatenated-member fuzz seed is retained.
The original rejection was reproduced before the fix; these checks supplement
the official suite rather than changing its exclusions or expected results.

Connect's ordered compression list now respects identity before gzip for POST,
eligible GET and streaming, while preserving inference from request encoding
when the list is omitted. A regression failed before the fix. The two new tests
cover both compressed/identity requests and preserve gRPC/gRPC-Web's distinct
capability-list semantics. All 222-test matrices above include this change.
The final ASan/UBSan parser campaign completed 5,798,617 inputs in 121 seconds
without a finding, including the new encoding-preference seed and parser path.
The requirement-by-requirement normative/API audit is in RELEASE_AUDIT.md.

## Full compliance gate

The objective is reproducible full compatibility for the scope in PLAN.md,
not a green subset presented as a verified implementation. Before completion:

- Run both client and server modes across all promised protocol/HTTP/RPC shapes.
  HTTP/2 Connect, gRPC and binary gRPC-Web are green; text gRPC-Web has local
  coverage. HTTP/1.1 non-bidi Connect/gRPC-Web and eligible Connect GET on
  both HTTP versions are green. Protocol-invalid combinations
  must be rejected explicitly and tested, not silently downgraded.
- Retain codec, compression, TLS/mTLS, limits, metadata, typed errors, cancellation,
  timeout and malformed-wire cases when expanding profiles. No unexplained
  exclusions or known failures may be hidden by runner configuration.
- Check normative protocol requirements in addition to the suite. Preserve
  targeted regression tests for gaps or ambiguities found during implementation.
- Execute Linux GCC/Clang, macOS Apple Clang and Windows MSVC CI, sanitizer/TSan
  and fuzzing gates, generated API compilation and installed-package consumers.
- Publish the exact suite revision, feature matrix and repeatable test commands
  in release artifacts. Passing these checks is compatibility evidence; it does
  not itself claim endorsement from the ConnectRPC maintainers.
