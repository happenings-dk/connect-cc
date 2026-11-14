# Contributing to Connect for C++

The project is an independent, pre-release implementation. The
[README](README.md) describes its API and protocol scope;
[project status](docs/PROGRESS.md) and the [release audit](docs/RELEASE_AUDIT.md)
distinguish implemented behavior from outstanding verification.

## Development setup

Follow [Getting started](docs/GETTING_STARTED.md) and use the pinned dependency
baseline in [DEPENDENCIES.md](docs/DEPENDENCIES.md). Build Google's generated
messages with the same Protobuf version as the C++ runtime. Keep compiler,
package-manager and sanitizer configurations in separate build directories.

For an existing configured development build, enable official conformance:

```sh
cmake -S . -B build -DCONNECTCPP_BUILD_CONFORMANCE=ON \
  -DCONNECTCPP_BUILD_GENERATOR=ON
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
```

The complete configuration currently contains 236 CTest entries. Ten entries
run the official client/server profiles, comprising 8,698 individual cases.
Do not add those case counts to the CTest entry count.

## Making a change

Keep changes focused and explain the observable behavior they affect. For a
protocol or concurrency fix, first reproduce the failure, then add a regression
that exercises the relevant boundary: malformed input, cancellation timing,
partial reads/writes, flow control or final status. Preserve all supported
protocols and transport combinations. Do not hide failures with skip patterns
or relax assertions to accommodate a compiler or platform.

The runtime returns RPC and transport failures as status values. Applications
own executors and may use `ManagedTransport` for connection establishment;
do not introduce hidden worker
threads, retries or custom coroutine promises. Streaming code must preserve
bounded memory, one concurrent reader/writer, backpressure and joined cleanup.
Public ownership contracts belong in the headers and relevant guide.

Use the repository's Google-based `.clang-format` for C++, idiomatic Go and
`gofmt` for the generator. Keep generated fixtures consistent with their schemas.
The C++ namespace is `connectrpc`; the CMake package/target is
`connectcpp` / `connectcpp::connectcpp`; the generator is
`protoc-gen-connect-cpp`. These names are intentionally different.

## Checks by change type

| Change | Checks |
| --- | --- |
| Wire encoding, transport or RPC behavior | Relevant local regressions, all affected official client/server profiles, then the full CTest suite |
| Concurrency, cancellation or lifetime | Normal tests plus separate ASan/UBSan and TSan builds; see [sanitizers](docs/SANITIZERS.md) |
| Generator or schemas | Go race tests, vet, staticcheck, golden output and compiled generated-binding tests |
| Public API, dependencies or packaging | Installed consumer/callback examples and independent compilation of every public header; relevant package recipes |
| Parser bounds or malformed input | Regression plus the [fuzz harness](docs/FUZZING.md) |
| Performance | Reproducible [benchmarks](docs/BENCHMARKS.md), with raw results and workload limitations |
| Documentation | Validate commands/API examples and local links; avoid unverified support or certification claims |

Focused commands, followed by the broader checks appropriate to the change:

```sh
ctest --test-dir build -R 'Http1Connection' --output-on-failure
ctest --test-dir build -L conformance --output-on-failure
go test -race ./...
go vet ./...
go run honnef.co/go/tools/cmd/staticcheck@v0.8.1 ./...
gofmt -l cmd internal
actionlint
```

Use an isolated `GOCACHE="$PWD/build/go-cache"` if concurrent local tooling
invalidates a shared Go cache. This changes only the cache location, not test
coverage. See [Code generation](docs/CODEGEN.md) for Buf/protoc parity checks.

CI checks Linux GCC/Clang, Linux arm64, macOS, Windows MSVC, ASan/UBSan and TSan.
Package recipes, parser fuzzing and the benchmark harness have additional jobs.
A configured lane is not proof of a successful run; retain the source revision
and CI link when reporting results. The current evidence is in
[HOSTED_VERIFICATION.md](docs/HOSTED_VERIFICATION.md).

## Reporting a problem

Include the source revision, OS/architecture, compiler, dependency versions,
build options and a minimal schema or reproducer. For an RPC failure, identify
the protocol, HTTP version, codec, compression and expected versus observed
status. Attach the failing test name or conformance profile and relevant logs;
remove credentials and application data first.

For changes proposed through a pull request, describe the problem, resulting
behavior, validation and any remaining limitations. There is no stable ABI
promise before release, but breaking public API changes should still be explicit
and update their examples and documentation.

## License

The project uses the [Apache License 2.0](LICENSE). Contributions are made under
the same license.
