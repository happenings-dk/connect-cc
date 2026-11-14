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
| Transport | Managed URL dialing/pooling, Asio TCP/TLS, native macOS/Windows trust, verified certificates and hostnames, ALPN, mTLS, flow control, cancellation, deadlines and bounded shutdown |
| Application API | Typed clients and coroutine handlers, callback completion, client/server interceptors |
| Web integration | Eligible Connect GET requests and configurable CORS middleware |
| Generation | Go protoc plugin, Buf integration, proto2/proto3, Editions 2023–2024 and Lite message classes |
| Distribution | CMake install/export, local vcpkg and Conan recipes, independent consumers |
| Verification | Official client/server adapters, regression tests, Go interoperability, sanitizers, parser fuzzing and reproducible benchmarks |

See the [documentation index](README.md) for API and protocol guides.

## Verification and remaining work

The pinned official matrix contains 4,822 client and 3,876 server cases across
ten CTest profiles. The expanded local macOS suite passes **236/236** in both normal and ASan/UBSan builds. A separate Lite build passes
its generated-binding/transport tests and an installed consumer without linking
full Protobuf. The native macOS system-trust HTTPS probe passes.

Hosted verification of the support expansion is in progress, including a strict
Linux sanitizer rerun after correcting OpenSSL's dependency instrumentation.
[HOSTED_VERIFICATION.md](HOSTED_VERIFICATION.md) records exact runs;
prior platform results are historical, not evidence for untested new changes.
Public release, registry publication and upstream adoption remain separate
steps. No official certification or maintainer endorsement is claimed.

## API boundaries

- ManagedTransport owns URL dialing, pooling and connection tasks. Applications
  own server listeners and executor threads and await its Close operation.
  Lower-level socket/connection APIs remain available.
- RPC errors are values. Coroutine/callback users share the same transport and
  protocol implementation; callbacks still require a C++20 compiler.
- Stream consumers read through EOF and inspect final status. A successful read
  or a provisional client-streaming response alone does not establish RPC success.
- Cancellation is cooperative. Blocking application code cannot be forcibly
  interrupted; handlers must join child work before returning.
- TLS defaults to native macOS/Windows policy or OpenSSL system roots on other
  platforms. Explicit CA overrides use OpenSSL. Shutdown exchanges close-notify
  within a deadline; Cancel is an abort.
- The generator supports proto2/proto3 and Editions 2023–2024. Lite messages
  support binary encoding; full message classes are required for protobuf JSON.
- Unary client body limits currently apply in both directions; streaming and
  server request/response limits can be configured independently.
- No stable API or binary ABI is promised before release. Pin the source and
  rebuild with matching compiler, dependency and Protobuf versions.

HTTP/3, Bazel, custom Protobuf runtimes, Unreal/mobile/console integrations and
additional platform combinations are outside the accepted initial scope.
