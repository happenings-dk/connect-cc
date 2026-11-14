# Connect for C++ documentation

Start with [Getting started](GETTING_STARTED.md) to build the library, generate
a service and connect its client and handlers to an Asio transport. The
[project README](../README.md) describes the supported protocols and current
release status.

## Using the library

| Guide | What it covers |
| --- | --- |
| [Getting started](GETTING_STARTED.md) | Build/install, a first schema, generated calls and transport setup |
| [Managed transport](MANAGED_TRANSPORT.md) | URL dialing, connection reuse, capacity, deadlines and joined shutdown |
| [Protobuf Lite](PROTOBUF_LITE.md) | Binary message codecs, Lite-only builds and package options |
| [Code generation](CODEGEN.md) | protoc/Buf, generated signatures, namespaces, schema support and GET eligibility |
| [Streaming](STREAMING.md) | All RPC shapes, byte/message streams, backpressure, lifetime and shutdown |
| [Callbacks](CALLBACKS.md) | Completion tokens, all four shapes, executors, allocators and cancellation |
| [Interceptors](INTERCEPTORS.md) | Authentication, metadata hooks, observation and per-call ownership |
| [Examples](../examples/README.md) | Executable examples built against the installed package |

## Protocols and transport

| Guide | What it covers |
| --- | --- |
| [HTTP/1.1](HTTP1.md) | Keep-alive, chunking, stream restrictions and connection lifecycle |
| [TLS negotiation](TLS_NEGOTIATION.md) | Certificate verification, ALPN, HTTP version selection and mTLS |
| [gRPC](GRPC.md) | HTTP/2, deadlines, status, trailers and typed error details |
| [gRPC-Web](GRPC_WEB.md) | Binary/text framing, incremental base64 and body trailers |
| [Connect GET](GET.md) | Side-effect-free methods, deterministic query encoding and limits |
| [Browser CORS](CORS.md) | Allowed origins, preflight, credentials and exposed metadata |

Public API contracts are documented in [the installed headers](../include/connectrpc/):
[`rpc.h`](../include/connectrpc/rpc.h) for typed calls and routing,
[`http_connection.h`](../include/connectrpc/http_connection.h) for connection
management, [`message_stream.h`](../include/connectrpc/message_stream.h) for
stream operations, and [`status.h`](../include/connectrpc/status.h) for errors.
The runtime does not currently publish a separate generated API-reference site.

## Building and verifying

| Guide | What it covers |
| --- | --- |
| [Dependencies](DEPENDENCIES.md) | Pinned versions, supported compiler baseline and ABI constraints |
| [Packaging](PACKAGING.md) | Local vcpkg/Conan recipes and independent CMake consumers |
| [Conformance](CONFORMANCE.md) | Official suite revision, capability profiles and reproduction commands |
| [Hosted verification](HOSTED_VERIFICATION.md) | Executed compiler/platform checks and links to CI evidence |
| [Sanitizers](SANITIZERS.md) | Separate ASan/UBSan and TSan builds, dependency instrumentation |
| [Fuzzing](FUZZING.md) | Parser harness, corpus, limits and campaign results |
| [Benchmarks](BENCHMARKS.md) | Reproduction, raw measurements and comparison limits |
| [Contributing](../CONTRIBUTING.md) | Development and review workflow |

## Project scope

[Project status](PROGRESS.md) summarizes implementation boundaries and open
release gates. [Release acceptance](RELEASE_AUDIT.md) maps requirements to
evidence. [PLAN.md](PLAN.md) preserves the accepted design and scope; it is not
an installation guide or a claim that every gate has passed.

For the language-independent wire contract, read the
[Connect protocol specification](https://connectrpc.com/docs/protocol/).
This repository is an independent implementation and does not imply
ConnectRPC project membership or certification.
