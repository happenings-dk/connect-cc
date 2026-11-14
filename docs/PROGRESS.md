# Project status

Connect for C++ is an independent, pre-release client/server implementation.
The accepted scope is preserved in [PLAN.md](PLAN.md), and the
[release audit](RELEASE_AUDIT.md) maps each requirement to its implementation and
evidence. The repository is hosted privately at
[happenings-dk/connect-cc](https://github.com/happenings-dk/connect-cc).

## Implemented

| Area | Capability |
| --- | --- |
| Protocols | Connect, gRPC and binary/text gRPC-Web |
| RPC shapes | Unary, server streaming, client streaming and bidirectional streaming |
| HTTP | HTTP/1.1 for Connect/gRPC-Web non-bidi calls; HTTP/2 for all shapes and protocols |
| Messages | Google C++ Protobuf, protobuf JSON, bounded gzip, typed errors, metadata and trailers |
| Transport | Asio TCP/TLS, verified certificates and hostnames, ALPN, mTLS, flow control, cancellation, deadlines and bounded shutdown |
| Application API | Typed clients and coroutine handlers, callback completion, client/server interceptors |
| Web integration | Eligible Connect GET requests and configurable CORS middleware |
| Generation | Go protoc plugin, Buf integration, proto2/proto3 full-runtime message classes |
| Distribution | CMake install/export, local vcpkg and Conan recipes, independent consumers |
| Verification | Official client/server adapters, regression tests, Go interoperability, sanitizers, parser fuzzing and reproducible benchmarks |

See the [documentation index](README.md) for API and protocol guides.

## Verification and remaining work

The pinned official matrix contains 4,822 client and 3,876 server cases across
ten CTest entries. The complete suite contains 222 entries. Linux GCC/Clang,
Linux arm64, macOS, ASan/UBSan and TSan runs have passed, along with installed
consumers and public-header checks. These results have explicit source
revisions in [HOSTED_VERIFICATION.md](HOSTED_VERIFICATION.md).

**Windows MSVC now passes the full suite and installed-package checks.** The
[corrected hosted run](https://github.com/happenings-dk/connect-cc/actions/runs/43532212334)
uses runtime revision `72d2739`. It fixes connection resets observed in the
first executable Windows run, without weakening protocol assertions. The same
correction passes 222/222 locally on Linux GCC/Clang/ASan/TSan and macOS normal
and ASan/UBSan. Hosted macOS, Linux GCC/Clang, Linux arm64 and TSan reruns also
pass. The refreshed hosted ASan/UBSan lane is still finishing and is tracked in
the verification report.

The hosted benchmark harness is being rerun after a cold gRPC dependency
build exhausted its time budget. Historical measured results remain labeled by
revision; a pending run is not counted as successful.

The required platform families now have passing evidence. Preserve exact
revisions and artifacts, and distinguish pending repeat checks from completed
runs. Public release, package-registry publication and upstream adoption
are separate steps. No official ConnectRPC certification or endorsement is
claimed.

## API boundaries

- Applications own DNS, TCP connection establishment, connection pooling,
  listener lifetime, executor threads and joining their operations.
- RPC errors are values. Coroutine/callback users share the same transport and
  protocol implementation; callbacks still require a C++20 compiler.
- Stream consumers read through EOF and inspect final status. A successful read
  or a provisional client-streaming response alone does not establish RPC success.
- Cancellation is cooperative. Blocking application code cannot be forcibly
  interrupted; handlers must join child work before returning.
- TLS uses OpenSSL trust configuration. Native OS certificate-store integration
  and a public TLS close-notify convenience are not implemented.
- The generator supports proto2/proto3 with the full Protobuf runtime. Editions
  and lite-runtime schemas are explicitly rejected.
- Unary client body limits currently apply in both directions; streaming and
  server request/response limits can be configured independently.
- No stable API or binary ABI is promised before release. Pin the source and
  rebuild with matching compiler, dependency and Protobuf versions.

HTTP/3, Bazel, custom Protobuf runtimes, Unreal/mobile/console integrations and
additional platform combinations are outside the accepted initial scope.
