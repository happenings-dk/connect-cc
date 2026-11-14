---
name: connect-cpp
description: Build, integrate, test, and debug C++ clients and servers using happenings-dk/connect-cc, its connectcpp SDK CLI, protoc-gen-connect-cpp, generated bindings, and CMake package. Use for Connect C++ projects and Connect/gRPC/gRPC-Web integration in C++. Combine with the general ConnectRPC and C++ style skills when available.
---

# Connect C++

This skill is for `happenings-dk/connect-cc`, an independent C++20 implementation.
Do not describe it as an official ConnectRPC release or endorsed implementation.
A conformance result applies to its tested revision, profiles, and configuration;
check the repository's current evidence before quoting counts or claiming support.

## Locate and verify

Run `connectcpp --help` and `connectcpp doctor` if installed. Read the target
project's CMake, dependency baseline, proto files, and instructions. Use a pinned
source revision and matching Protobuf compiler/runtime; do not mix incompatible
binary toolchains. The runtime is compiled C++, not a header-only Go module.

Canonical source and docs: https://github.com/happenings-dk/connect-cc
Read `docs/INSTALLATION.md` for SDK setup, `docs/CODEGEN.md` for binding details,
`docs/MANAGED_TRANSPORT.md` for dialing/lifecycle, and `docs/STREAMING.md` when
working on streaming. Read the corresponding local files when a checkout exists.
Repository access may require GitHub authentication; do not change visibility or
publish packages to resolve access errors without user authorization.

## New project

With the SDK already set up:

```sh
connectcpp new hello
cd hello
connectcpp build
./build/greeter serve
# Another terminal:
./build/greeter call http://127.0.0.1:8080 Ada
```

Windows uses `build\greeter.exe` in a compiler developer terminal.
`new` requires a nonexistent directory and creates ordinary CMake/proto/C++ files.
`build` configures, builds, and runs CTest. Use `--prefix PATH` before the project
argument to select an SDK, or set `CONNECTCPP_ROOT`. It does not fetch updates.

If installing from a checkout, first follow the dependency setup in
`docs/INSTALLATION.md`, then run `go run ./cmd/connectcpp setup --source .`.
Go builds the development tools; it is not a C++ runtime dependency.
Do not promise a registry package or release that has not been published.

## Bindings and application changes

Use `find_package(connectcpp CONFIG REQUIRED)` and the exported target
`connectcpp::connectcpp`. `connectcpp_generate(TARGET ... PROTO_ROOT ...
SERVICES ... MESSAGES ...)` produces message classes and service bindings.
`SERVICES` lists schemas declaring services, `MESSAGES` lists imported message-only
schemas needing compilation; paths are relative to PROTO_ROOT. Generated outputs
belong in the build tree. Do not manually edit them. Import roots can be added with
`IMPORT_DIRS`, but imports needing C++ objects must also be generated or linked.
For Protobuf Lite, follow `docs/PROTOBUF_LITE.md`; JSON application messages need
the full runtime and Lite generation options must match the linked runtime.

Messages use the proto package namespace. Bindings use
`connectrpc::generated::<proto-package>` with `ServiceNameClient`,
`ServiceNameHandler`, and `RegisterServiceNameHandler`. Copy an actual generated
signature before implementing a handler; do not infer it from another language.

## Runtime contracts

- Use `ManagedTransport::Create(executor, base_url, options)` for URL dialing,
  TLS, and bounded connection pools. Check its result, keep the transport alive
  while its client is in use, and await `Close(deadline)` before stopping the loop.
- Choose the cleartext HTTP version explicitly. The starter uses HTTP/1.1;
  the transport default is HTTP/2 prior knowledge. Bidi requires HTTP/2. Protocol
  selection and HTTP version are separate; inspect the actual API options.
- Use the project's established Asio coroutine or callback model. `Async` is
  the callback bridge. One stream supports at most one pending read and one
  pending write; join both directions before final completion/cleanup.
- `ServeHttpConnection` owns the connection's run task and joins handlers.
  Do not start `Run` separately when using it. Keep registered handlers alive.
- Set deadlines and resource limits. Distinguish cancellation requests from
  completed cleanup. Pool saturation fails instead of silently queuing forever;
  the managed transport does not retry RPCs.
- Respect executor/strand requirements. A shared pointer protects lifetime,
  not thread safety. Avoid dangling coroutine captures and references.
- Preserve status codes, error details, headers, and trailers. Do not turn an
  RPC error into a successful empty message. Use verified TLS for remote traffic;
  do not disable certificate checks to make an example work.

## Verification

Build the real generated project and run its tests after integration changes.
For runtime/protocol work, use the repository's tests and pinned official
conformance profiles. Report exactly what ran. Protocol, transport, runtime
variant, and platform limits belong in the result. Do not claim full conformance
from a greeting test or claim all-platform support from one local build.
