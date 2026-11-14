# Conformance and interoperability

The runtime is tested against the official
[ConnectRPC conformance suite](https://github.com/connectrpc/conformance) in
both client and server modes. The configured profiles cover Connect, gRPC and
gRPC-Web over HTTP/2, plus Connect and gRPC-Web over HTTP/1.1, with Protobuf/JSON,
identity/gzip, cleartext, TLS and mTLS.

**The matrix contains 8,698 cases: 4,822 client and 3,876 server cases.** These
pass on verified Linux, macOS and Windows MSVC builds;
[HOSTED_VERIFICATION.md](HOSTED_VERIFICATION.md) records platform results and
source revisions. Passing a pinned capability matrix is not official
certification or proof that every possible protocol input is correct.

## Coverage

| Profile | Client cases | Server cases |
| --- | ---: | ---: |
| [Connect unary POST/GET](../conformance/unary.yaml) | 394 | 416 |
| [Connect streaming](../conformance/streaming.yaml) | 576 | 554 |
| [gRPC, all shapes](../conformance/grpc.yaml) | 1,084 | 876 |
| [gRPC-Web binary, all shapes](../conformance/grpc_web.yaml) | 1,110 | 696 |
| [HTTP/1.1 Connect and gRPC-Web](../conformance/http1.yaml) | 1,658 | 1,334 |
| **Total** | **4,822** | **3,876** |

HTTP/2 covers unary, server streaming, client streaming and half-/full-duplex
bidirectional streaming. HTTP/1.1 covers the three non-bidi shapes; native gRPC
and bidi require HTTP/2 and are rejected on HTTP/1.1. Eligible Connect GET calls
are covered on both HTTP versions.

The profiles declare supported capabilities without skip or known-failure
patterns. They are not a claim to implement every optional capability in the
upstream suite. gRPC-Web text framing and CORS have separate local coverage;
they are not counted as official binary gRPC-Web or browser certification cases.

## Pinned suite

The source revision is
[`73e45c4dda9c133fab1538d6d6c6d66c7deed844`](https://github.com/connectrpc/conformance/tree/73e45c4dda9c133fab1538d6d6c6d66c7deed844).
[CMake](../cmake/Conformance.cmake) verifies the source archive, builds the runner
and generates C++ messages from the same checkout. The suite's Go module lock
controls its reference implementation dependencies.

The [C++ adapter](../conformance) communicates through the suite's four-byte,
big-endian, size-delimited Protobuf stdin/stdout format. Input/output frames are
bounded to 64 MiB. Only protocol messages go to stdout; diagnostics go to stderr.
The adapters use the public client API and shared server routing paths.

## Run the matrix

Starting from a build configured with the appropriate dependency toolchain:

```sh
cmake -S . -B build -DCONNECTCPP_BUILD_CONFORMANCE=ON \
  -DCONNECTCPP_BUILD_GENERATOR=ON
cmake --build build --parallel 3
ctest --test-dir build -L conformance --output-on-failure
```

This downloads the pinned suite and Go dependencies. Go 1.26+ is required.
Conformance is off by default for runtime-only consumers.

CTest executes client and server modes for each of the five profiles: **ten
CTest entries**, within the full **235-entry** test suite. The 8,698 official
cases are nested within those entries and must not be added to the CTest count.

For diagnosis, select an individual profile and case family:

```sh
build/bin/connectconformance --mode client --conf conformance/unary.yaml \
  --run 'Client Cancellation/**' -- build/connectcpp_conformance client
build/bin/connectconformance --mode server --conf conformance/unary.yaml \
  --run 'Deadline Propagation/**' -- build/connectcpp_conformance server
```

On Windows, the executables have an `.exe` suffix. Focused cases help reproduce
failures; release evidence must include the complete promised profiles.

## Tests beyond the official suite

| Area | Additional coverage |
| --- | --- |
| Generated bindings | Golden output, malformed generator requests, naming collisions, imported/nested/optional messages, all shapes and callback calls |
| Cross-language RPC | C++ client to connect-go and connect-go client to C++, both codecs, compression, metadata, trailers, large messages and typed errors |
| Framing and limits | Fragmented/coalesced input, malformed headers/envelopes, bounded allocation/decompression, concatenated gzip members and ordered encoding preferences |
| Transport lifecycle | Multiplexing, flow control, slow peers, early responses, resets, deadline/cancellation races, trailers, queue bounds and joined shutdown |
| HTTP/1.1 | Sequential reuse, chunking, close-delimited bodies, early response during upload, invalid framing and empty Transfer-Encoding rejection |
| TLS | Chain/DNS/IP verification, ALPN policy matrix, mTLS, handshake deadlines/cancellation and negotiated engine handoff |
| gRPC-Web text | Incremental base64 segmentation, body trailers, malformed terminal data and supported TCP/TLS RPC shapes |
| Browser CORS / GET | Preflight, origins, credentials, exposed metadata, eligible methods, cache keys and bounded URL decoding |
| API integration | Interceptor lifecycle, associated callback executors/allocators, cancellation timing, installed consumers and 21 independently compiled public headers |

`Interop.Go` uses connect-go v1.21.1 from this repository's Go lockfile. The
official suite uses its own pinned reference dependencies. The Go race-enabled
interop helper is compiled during the build, so cold compiler-cache time does
not consume its runtime test deadline.

Additional stress runs are recorded separately from the official case count.
For example, `ci/stress-timeouts.sh build-linux-clang 20` exercises 1,120 repeated
unary/HTTP/1.1 timeout cases. Normative review also produced regressions for
concatenated gzip members and Connect's ordered compression preferences, beyond
the suite's cases. See the [release audit](RELEASE_AUDIT.md).

## Verification boundaries

The [hosted report](HOSTED_VERIFICATION.md) links executed compiler checks,
original failures and corrected runs. [Sanitizers](SANITIZERS.md),
[fuzzing](FUZZING.md) and [packaging](PACKAGING.md) document their separate
requirements. Configuring a CI lane does not establish a pass; results apply to
the recorded revision and environment.

Before declaring the accepted release scope complete:

- Pass client and server profiles for every promised protocol, HTTP version,
  codec, compression and RPC shape, with unsupported combinations rejected.
- Preserve local regressions and normative review for behavior outside the suite.
- Pass the required compiler/platform matrix, ASan/UBSan, TSan, parser fuzzing,
  generated API checks and independent installed consumers.
- Retain the suite revision, capability matrix, logs and reproduction commands.

[RELEASE_AUDIT.md](RELEASE_AUDIT.md) tracks these gates. Upstream adoption or an
official verification designation would require a separate upstream process;
this repository does not claim either.
