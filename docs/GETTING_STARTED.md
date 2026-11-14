# Getting started

This guide builds Connect for C++, generates a small service, and shows how to
attach its client and handler to an application-owned Asio connection. Run shell
commands from the repository root unless stated otherwise.

The project is pre-release. Pin the source revision you use, and consult
[hosted verification](HOSTED_VERIFICATION.md) before selecting a platform.

## 1. Build and install

You need CMake 3.25+, Ninja, a C++20 compiler and the dependencies listed in
[DEPENDENCIES.md](DEPENDENCIES.md). The Linux compiler baseline uses GCC 14 or
Clang 18. Go 1.26+ builds `protoc-gen-connect-cpp`; generated applications do
not link or run Go.

With dependencies already installed and discoverable by CMake:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCONNECTCPP_BUILD_GENERATOR=ON
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/install"
```

On Apple Silicon with Homebrew, you may need
`-DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/libnghttp2"`.

For a pinned Linux dependency build, use the repository's vcpkg baseline and
release triplet instead:

```sh
git clone https://github.com/microsoft/vcpkg.git build/vcpkg
git -C build/vcpkg checkout 3d22b64585ca026e881728fd87e541c23a611a43
./build/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export CC=gcc-14 CXX=g++-14
cmake -S . -B build-vcpkg -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/ci/triplets" \
  -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_HOST_TRIPLET=x64-linux \
  -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg-installed" \
  -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release \
  -DCONNECTCPP_BUILD_GENERATOR=ON -DCONNECTCPP_BUILD_CONFORMANCE=ON
cmake --build build-vcpkg --parallel 3
ctest --test-dir build-vcpkg --output-on-failure
cmake --install build-vcpkg --prefix "$PWD/install"
```

This configuration includes the official protocol tests. Their source and Go
dependencies are downloaded during configuration/build. The first dependency
build can take substantial time; subsequent builds can use vcpkg's binary cache.

The compiler/platform settings above are for Linux x86-64. The
[CI matrix](../.github/ci-matrix.json) records the macOS, Linux arm64 and Windows
configurations; Windows uses `bootstrap-vcpkg.bat` in a developer shell.
[Packaging](PACKAGING.md) covers Conan and building a distributable vcpkg port.
Do not reuse a build directory across different compilers or toolchains.

## 2. Define a service

Save this as `build/tutorial/proto/greet.proto`, creating its parent directories:

```protobuf
syntax = "proto3";
package greet.v1;

service GreetService {
  rpc Greet(GreetRequest) returns (GreetResponse);
}

message GreetRequest { string name = 1; }
message GreetResponse { string greeting = 1; }
```

The generated message classes live in `greet::v1`. Service bindings live in
`connectrpc::generated::greet::v1`.

## 3. Generate C++ bindings

Use **the `protoc` version matching your linked Protobuf runtime**. For the
vcpkg build above, it is `build/vcpkg-installed/x64-linux/tools/protobuf/protoc`.
With that executable on `PATH`:

```sh
mkdir -p build/tutorial/generated
protoc --proto_path=build/tutorial/proto \
  --cpp_out=build/tutorial/generated \
  --plugin=protoc-gen-connect-cpp="$PWD/install/bin/protoc-gen-connect-cpp" \
  --connect-cpp_out=build/tutorial/generated \
  greet.proto
```

This produces `greet.pb.h`, `greet.pb.cc`, `greet.connect.h` and
`greet.connect.cc`. Compile both `.cc` files into your application.

The generator can also be built separately with Go:

```sh
go build -trimpath -o bin/protoc-gen-connect-cpp ./cmd/protoc-gen-connect-cpp
```

For Buf users, this local-plugin template produces the same bindings:

```yaml
version: v2
plugins:
  - protoc_builtin: cpp
    out: build/tutorial/generated
  - local: bin/protoc-gen-connect-cpp
    out: build/tutorial/generated
```

Save it as `build/tutorial/buf.gen.yaml`, then run
`buf generate build/tutorial/proto --template build/tutorial/buf.gen.yaml`.
The local `protoc` must still match the runtime. There is no published Buf
remote plugin for this project. See [Code generation](CODEGEN.md) for streaming
signatures, naming rules and supported schema features.

## 4. Implement a handler

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

A default `Status` is successful. To return an RPC error, set `response.status`
and leave the message absent. Requests carry leading metadata and a monotonic
deadline; responses can carry both headers and trailers.

Register before accepting RPCs:

```cpp
Greeter greeter;
connectrpc::Server server;
auto status = connectrpc::generated::greet::v1::RegisterGreetServiceHandler(
    server, greeter);
// Check status.ok() before starting the serving operation.
```

Keep `greeter` and `server` alive until every serving operation has joined.

## 5. Connect the runtime to Asio

Applications own DNS, TCP connection establishment, listener acceptance and the
event loop. Given an already-connected `asio::ip::tcp::socket`, create an
`HttpConnection` with the appropriate role and explicit cleartext HTTP version:

```cpp
auto created = connectrpc::HttpConnection::Create(
    std::move(socket), connectrpc::HttpConnection::Role::kClient,
    connectrpc::HttpVersion::kHttp2);
if (!created.ok()) co_return created.status();
auto connection = *created.value();
```

The snippet belongs inside an application coroutine returning
`asio::awaitable<connectrpc::Status>`. HTTP/2 cleartext uses prior knowledge;
use `kHttp1` for an HTTP/1.1 peer. For HTTPS, configure an OpenSSL context and
use `HttpConnection::CreateTls` as described in [TLS negotiation](TLS_NEGOTIATION.md).
TLS verifies the peer and hostname and selects HTTP/2 or HTTP/1.1 through ALPN.

On a client connection, start `connection->Run()` once as an asynchronous
operation on your event loop and keep it running while RPCs are active. Adapt
that connection to the RPC client:

```cpp
connectrpc::Client runtime(
    {}, [connection](connectrpc::Metadata headers,
                     connectrpc::TransportOptions options) {
      headers.emplace(headers.begin(), ":authority", "localhost");
      headers.emplace(headers.begin(), ":scheme", "http");
      return connection->Open(std::move(headers), options.deadline);
    });
```

Use the actual server authority, including its port when needed, and `https`
for TLS. Pseudo-headers must precede ordinary headers. The exchange supplies
transport; the generated client supplies the method, message and call options.

For each accepted server socket, create a server-role connection and run:

```cpp
auto served = co_await connectrpc::ServeHttpConnection(
    connection, [&server](std::shared_ptr<connectrpc::ByteStream> stream) {
      return server.HandleHttpStream(std::move(stream));
    });
```

`ServeHttpConnection` owns the connection's I/O loop and joins its handler
tasks. Do **not** also start `Run()` on that server connection.
The [complete callback example](../examples/callbacks/main.cc) shows socket
setup, both endpoint roles, operation startup and completion joining in one
executable.

## 6. Make a typed call

Inside a coroutine, with the runtime client above:

```cpp
connectrpc::generated::greet::v1::GreetServiceClient client(runtime);
greet::v1::GreetRequest request;
request.set_name("World");

connectrpc::CallOptions options;
options.transport.deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(5);
auto response = co_await client.Greet(std::move(request), options);
if (!response.status.ok()) co_return response.status;
std::cout << response.message->greeting() << '\n';
co_return connectrpc::Status{};
```

Include `<chrono>`, `<iostream>`, `<memory>`, `<utility>`,
`"connectrpc/http_connection.h"` and `"greet.connect.h"` as required by the
snippets. Connect with binary Protobuf is the default. Set `options.codec` to
`Codec::kJson`, `options.compress` to `true`, or `options.protocol` to
`Protocol::kGrpc` / `Protocol::kGrpcWeb` when needed. Native gRPC requires HTTP/2.

The same generated call supports callback completion:

```cpp
connectrpc::Async(executor, client.Greet(std::move(request), options),
    [](connectrpc::Response<greet::v1::GreetResponse> response) {
      if (!response.status.ok()) {
        std::cerr << response.status.message << '\n';
        return;
      }
      std::cout << response.message->greeting() << '\n';
    });
```

Include `"connectrpc/callback.h"`. Keep the runtime, generated stub and captured
application state alive until completion. Callbacks do not create threads or
run the event loop for you. [Callbacks](CALLBACKS.md) explains executors,
cancellation slots and all four streaming patterns.

## 7. Link and shut down cleanly

A consumer CMake target needs the installed package and both generated sources:

```cmake
cmake_minimum_required(VERSION 3.25)
project(greeting LANGUAGES CXX)
find_package(connectcpp CONFIG REQUIRED)
add_executable(greeting main.cc generated/greet.pb.cc generated/greet.connect.cc)
target_include_directories(greeting PRIVATE generated)
target_link_libraries(greeting PRIVATE connectcpp::connectcpp)
```

Configure with `-DCMAKE_PREFIX_PATH=/absolute/path/to/install`. With vcpkg,
forward the same toolchain, installed directory and triplets used for the
library. The exported target carries its C++20 requirement and dependencies.

After completing calls, invoke `connection->Shutdown(deadline)` and keep the
event loop running until connection and serving operations finish. Applications
must also stop their listener and join their own tasks. A successful stream
read returning EOF is not an RPC success result: drain reads and check
`Finish().status`. Keep at most one read and one write in flight per stream.

Next: [streaming](STREAMING.md), [interceptors](INTERCEPTORS.md),
[Connect GET](GET.md), or [browser CORS](CORS.md). To reproduce the complete
protocol matrix, follow [Conformance](CONFORMANCE.md).
