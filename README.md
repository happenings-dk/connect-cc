# Connect for C++

[![CI](https://github.com/happenings-dk/connect-cc/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/happenings-dk/connect-cc/actions/workflows/ci.yml)
[![Packages](https://github.com/happenings-dk/connect-cc/actions/workflows/packages.yml/badge.svg?branch=main)](https://github.com/happenings-dk/connect-cc/actions/workflows/packages.yml)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](LICENSE)

**Typed RPC clients and servers for C++20, with Connect, gRPC and gRPC-Web.**

Define a service in Protocol Buffers, generate its C++ bindings, and implement
its handlers. `connect-cc` handles serialization, routing, streaming, compression,
metadata and RPC errors. The same service can communicate with clients and
servers across the [ConnectRPC ecosystem](https://connectrpc.com/).

The runtime uses standalone Asio, Google's C++ Protobuf runtime, nghttp2,
llhttp, OpenSSL and zlib. It integrates with your Asio event loop and exposes
both coroutine operations and callback completion.

**Status: experimental, pre-release.** This is an independent implementation,
not an official ConnectRPC project. APIs may change. The pinned official
conformance profiles pass 8,698 cases on Linux, macOS and Windows MSVC.
See the [verification report](docs/HOSTED_VERIFICATION.md)
for exact revisions, results and verification limits.

[Getting started](docs/GETTING_STARTED.md) · [Documentation](docs/README.md) ·
[Examples](examples/README.md) · [Contributing](CONTRIBUTING.md)

## Features

- **One service, three protocols.** Connect, gRPC and binary/text gRPC-Web use
  the same generated clients and handlers.
- **All four RPC shapes.** Unary, server streaming, client streaming and
  bidirectional streaming, with backpressure and bounded queues.
- **Protobuf and JSON.** Google's generated message classes, structured errors
  with typed details, leading metadata and trailers. Optional Lite-only builds
  support binary messages without full Protobuf reflection.
- **Asynchronous C++.** `co_await` with Asio, or callbacks through `Async`.
  Applications own their executors and listener lifecycle.
- **Managed connections.** URL dialing, DNS/connect/TLS deadlines, HTTP/2 reuse,
  bounded HTTP/1 pools and idle expiry, without automatic RPC retries.
- **Transport controls.** HTTP/1.1 and HTTP/2, verified TLS, ALPN, optional mTLS,
  deadlines, cancellation, message limits and bounded connection shutdown.
- **Application integration.** Client/server interceptors, gzip compression,
  cacheable Connect GET calls and configurable browser CORS middleware.
- **Build integration.** A Go `protoc` plugin, Buf code generation, an exported
  CMake target, local vcpkg and Conan recipes, and Editions 2023–2024 support.

| Protocol | HTTP/1.1 | HTTP/2 | Message encoding |
| --- | --- | --- | --- |
| Connect | Unary, server/client streaming | All four shapes | Protobuf, JSON |
| gRPC | — | All four shapes | Protobuf, JSON |
| gRPC-Web | Unary, server/client streaming | All four shapes | Protobuf, JSON; binary or base64 framing |

Bidirectional streaming requires HTTP/2. Browser APIs can impose additional
streaming restrictions; gRPC-Web support in this C++ runtime does not remove
those browser limitations.

## A small example

Start with a schema:

```protobuf
syntax = "proto3";
package greet.v1;

service GreetService {
  rpc Greet(GreetRequest) returns (GreetResponse);
}

message GreetRequest { string name = 1; }
message GreetResponse { string greeting = 1; }
```

`protoc-gen-connect-cpp` generates a typed client and an abstract handler next
to Google's message classes. Implement the handler:

```cpp
#include "greet.connect.h"

class Greeter final
    : public connectrpc::generated::greet::v1::GreetServiceHandler {
 public:
  asio::awaitable<connectrpc::Response<greet::v1::GreetResponse>> Greet(
      connectrpc::Request<greet::v1::GreetRequest> request) override {
    connectrpc::Response<greet::v1::GreetResponse> response;
    response.message.emplace().set_greeting("Hello, " + request.message.name());
    co_return response;
  }
};
```

Register it before serving connections, and check the returned status:

```cpp
Greeter greeter;
connectrpc::Server server;
auto status = connectrpc::generated::greet::v1::RegisterGreetServiceHandler(
    server, greeter);
```

On the client, create a managed transport on your Asio executor. Inside a coroutine:

```cpp
#include "connectrpc/managed_transport.h"

auto created = connectrpc::ManagedTransport::Create(
    co_await asio::this_coro::executor, "https://api.example.com");
if (!created.ok()) co_return created.status();
auto transport = *created.value();
auto runtime = transport->MakeClient();
connectrpc::generated::greet::v1::GreetServiceClient client(runtime);
greet::v1::GreetRequest request;
request.set_name("World");

connectrpc::CallOptions options;
options.transport.deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(5);
auto response = co_await client.Greet(std::move(request), options);
if (response.status.ok()) std::cout << response.message->greeting() << '\n';
auto closed = co_await transport->Close(
    std::chrono::steady_clock::now() + std::chrono::seconds(2));
co_return response.status.ok() ? closed : response.status;
```

The [getting started guide](docs/GETTING_STARTED.md) covers building, generating
these bindings and connecting the RPC API to Asio. The
[callback example](examples/callbacks/main.cc) is a complete executable with a
real client and server, all four RPC shapes, interceptors and joined shutdown.

## Build and use

Install the SDK once and create independent C++ projects with the `connectcpp`
CLI. With the prerequisites in the [installation guide](docs/INSTALLATION.md):

```sh
./install.sh                         # from this checkout; installs to ~/.local
export PATH="$HOME/.local/bin:$PATH"
connectcpp new hello
cd hello
connectcpp build                     # generates bindings, builds, and tests
./build/greeter serve                # another terminal: ./build/greeter call
```

Windows can use `install.ps1` from a compiler-enabled developer terminal.
The generated project is ordinary CMake, with a proto schema, typed client and
server, and a real round-trip test. `connectcpp skill install` installs the
companion agent skill. See [SDK installation](docs/INSTALLATION.md) for pinned
vcpkg setup, existing projects, custom prefixes, and upgrades.

Requirements: CMake 3.25+, a C++20 compiler, and the
[runtime dependencies](docs/DEPENDENCIES.md). Go 1.26+ is needed to build the
code generator and conformance tools, but is not a runtime dependency.
The pinned vcpkg baseline and Conan lockfile provide reproducible dependency
versions; [getting started](docs/GETTING_STARTED.md) gives the full setup.

With dependencies available to CMake:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCONNECTCPP_BUILD_GENERATOR=ON
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/install"
```

Consumers link the installed target:

```cmake
find_package(connectcpp CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE connectcpp::connectcpp)
```

Use the same Protobuf compiler version as the linked C++ runtime. Before a
stable release, pin a source revision and rebuild when compiler or dependency
versions change. Packages are currently built from source; no public package
registry or stable binary ABI is promised.

## Documentation

| Topic | Guide |
| --- | --- |
| Install the SDK and create another project | [SDK installation](docs/INSTALLATION.md) |
| Installation, code generation and first call | [Getting started](docs/GETTING_STARTED.md) |
| Generated clients and service handlers | [Code generation](docs/CODEGEN.md) |
| URL dialing, pooling and lifecycle | [Managed transport](docs/MANAGED_TRANSPORT.md) |
| Binary messages without reflection | [Protobuf Lite](docs/PROTOBUF_LITE.md) |
| Streaming, ownership and flow control | [Streaming](docs/STREAMING.md) |
| Callback completion and cancellation | [Callbacks](docs/CALLBACKS.md) |
| Authentication, metadata and observation | [Interceptors](docs/INTERCEPTORS.md) |
| HTTP/1.1, TLS and protocol negotiation | [HTTP/1.1](docs/HTTP1.md), [TLS](docs/TLS_NEGOTIATION.md) |
| Browser clients and cacheable requests | [CORS](docs/CORS.md), [Connect GET](docs/GET.md) |
| Distribution and dependency versions | [Packaging](docs/PACKAGING.md), [Dependencies](docs/DEPENDENCIES.md) |
| Protocol coverage and executed checks | [Conformance](docs/CONFORMANCE.md), [Verification](docs/HOSTED_VERIFICATION.md) |
| Performance and parser hardening | [Benchmarks](docs/BENCHMARKS.md), [Fuzzing](docs/FUZZING.md) |

The [documentation index](docs/README.md) also links protocol-specific guides,
public headers, sanitizer instructions and the release audit.

## Compatibility and project status

The official [ConnectRPC conformance suite](https://github.com/connectrpc/conformance)
is pinned to a source revision and run in both client and server modes. The
ten configured profiles cover 4,822 client and 3,876 server cases. They form
part of the full CTest suite, alongside generated-code, transport,
malformed-input and cross-language interoperability tests. Passing these
profiles is evidence of interoperability, not official certification or
maintainer endorsement.

Linux GCC/Clang, Linux arm64, macOS, Windows MSVC, ASan/UBSan and TSan checks
have passed. [Executed results](docs/HOSTED_VERIFICATION.md)
distinguish successful revisions from current work. See the
[release audit](docs/RELEASE_AUDIT.md) for the complete acceptance criteria.

The generator supports proto2, proto3 and Editions 2023–2024, including Lite
schemas. Managed transport provides URL dialing and pooling; native macOS and
Windows certificate policy is used by default, with OpenSSL system roots on
Linux. TLS shutdown exchanges close-notify within a deadline. Applications own
listener management and executor threads. Callbacks require a C++20 toolchain;
Lite application messages support binary encoding only.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow, relevant
test commands and how to report reproducible protocol failures. Changes to wire
behavior should include a regression and pass the affected official profiles.

## License

[Apache License 2.0](LICENSE).
