# C++ service generation

The Go plugin generates `.connect.h` and `.connect.cc` files next to Google's
`.pb.h`/`.pb.cc` messages. It reads ordinary protoc plugin requests and does not
require `go_package` or any Go-specific schema options.

```sh
go build -o bin/protoc-gen-connect-cpp ./cmd/protoc-gen-connect-cpp
buf generate tests/proto --template buf.gen.yaml
```

Equivalent protoc invocation (create the output directory first):

```sh
mkdir -p build/generated
protoc -I tests/proto \
  --cpp_out=build/generated \
  --plugin=protoc-gen-connect-cpp="$PWD/bin/protoc-gen-connect-cpp" \
  --connect-cpp_out=build/generated \
  test/v1/echo.proto shared/v1/types.proto
```

CMake builds the plugin and compiles generated fixtures when testing is enabled.
Set `CONNECTCPP_BUILD_GENERATOR=OFF` to build only the C++ runtime without Go.
The plugin is installed in `bin` when built.

For service `test.v1.Echo`, the generated API lives in
`connectrpc::generated::test::v1` and contains:

- `EchoClient`, constructed with a reference to a runtime `connectrpc::Client`.
- `EchoHandler`, an abstract coroutine interface to implement.
- `RegisterEchoHandler(server, handler)`, which checks every method for conflicts
  before registering any method. Register services before starting the server.

All referenced runtime clients, servers, and handlers must outlive their calls.
Message classes remain Google's generated classes in their normal namespaces.
Client methods return Asio awaitables. Unary and server-streaming methods accept
their single request by value; client-streaming and bidi methods accept only
`StreamCallOptions` and return an opened `TypedStream<Input, Output>` result.
Read through EOF and inspect `Finish` for the final RPC status.

Handler signatures follow the schema:

| Shape | Handler input | Handler return |
|---|---|---|
| Unary | `Request<Input>` | `Response<Output>` |
| Server streaming | `Request<Input>`, `TypedStream<Output, Input>` | `StreamCompletion` |
| Client streaming | `CallContext`, `TypedStream<Output, Input>` | `Response<Output>` |
| Bidirectional | `CallContext`, `TypedStream<Output, Input>` | `StreamCompletion` |

Requests and call contexts include metadata and the monotonic deadline.
Client-streaming handlers read requests and return the one response, without
calling `Write`. Server/bidi handlers write responses and return final status and
trailers, without calling `End` or `Finish`. Routing owns terminal response
flushing and cleanup. A stream must not survive its handler. Call
`SendHeaders(metadata)` before the first response write to set leading metadata.
For client streaming, response headers can be returned in `Response` instead.

C++ keywords receive a trailing underscore. Method names that collide with
another generated identifier gain `Rpc` suffixes until unique. Reserved C++
underscore patterns are encoded as `Rpc_` followed by hexadecimal name bytes.
Wire procedure names always preserve the exact original Protobuf names.
Include guards have a path digest so files such as `foo-bar.proto` and
`foo_bar.proto` cannot shadow one another.

Current limitations: proto2/proto3 and the full protobuf runtime only. Editions
and lite-runtime requests fail explicitly. Generated calls use the runtime's
transport-independent API: Connect, gRPC and gRPC-Web support all four shapes
over HTTP/2; Connect/gRPC-Web also support unary, server streaming and client
streaming over HTTP/1.1. Native gRPC and bidi require HTTP/2 and fail explicitly
on HTTP/1.1.

Validation includes exact golden output, malformed requests, unsupported options,
source path safety, deterministic output, keyword/name collisions, compilation
of imported/nested/optional messages, and calls through generated bindings for
all four RPC shapes, including a real loopback streaming client/server test.

```sh
go test -race ./...
go vet ./...
staticcheck ./...
cmake --build build
ctest --test-dir build --output-on-failure
```

## Protobuf 36 with a prebuilt runtime under ASan

On this machine, an ASan-instrumented Protobuf 36.1 generated message linked
against Homebrew's non-instrumented runtime crashes in
`TcParser::VerifyHasBitConsistency`, even in a standalone program that only
constructs and moves the message. No Connect code is needed to reproduce it.
Protobuf's `port_def.inc` switches its message globals representation when
`ABSL_HAVE_ADDRESS_SANITIZER` is set.

For this mixed build, use the upstream compatibility opt-out:

```sh
cmake -S . -B build-asan -DCONNECTCPP_SANITIZE=ON \
  -DCONNECTCPP_PROTOBUF_GLOBALS_COMPAT=ON
```

This retains ASan/UBSan instrumentation and selects the runtime-compatible
Protobuf globals representation. It is off by default: consistently instrumented
source builds of the dependencies should not silently inherit a compatibility
workaround intended for a prebuilt runtime.

## GET eligibility

Unary `NO_SIDE_EFFECTS` and `IDEMPOTENT` method options propagate into runtime
client calls and server registration. Only `NO_SIDE_EFFECTS` permits an opt-in
`CallOptions::use_get` request. Unannotated methods and merely idempotent methods
remain POST-only. Streaming methods do not expose GET. A generated binding test
executes an annotated method and verifies ineligible calls never reach transport.
See [GET.md](GET.md) for encoding, limits and usage.
