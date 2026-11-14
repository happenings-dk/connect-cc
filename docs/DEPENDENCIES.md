# Reproducible dependency baseline

The vcpkg baseline below and the separately locked Conan graph are both tested.
See PACKAGING.md for package-manager commands and the differences in their
nghttp2/llhttp versions.

The development manifest `vcpkg.json` selects the registry revision
`3d22b64585ca026e881728fd87e541c23a611a43`. Use that same vcpkg checkout in CI and
local builds; do not silently update the registry or bootstrap tool. Every port
in this checkout records its source revision and archive checksum.

| Dependency | Selected version |
| --- | --- |
| Abseil | 20260107.1, port revision 3 |
| Asio | 1.38.2 |
| GoogleTest | 1.18.0 |
| llhttp | 9.4.3 |
| nghttp2 | 1.71.0 |
| OpenSSL | 3.6.5, port revision 2 |
| Protobuf | 7.36.2, port revision 1 |
| zlib | 1.4 |
| pkgconf (build host) | 3.0.3, port revision 1 |

The optional `benchmarks` manifest feature adds native gRPC C++ 1.84.0 and its
registry-pinned dependencies. It uses a separate installed directory in
`ci/run-benchmarks.sh`; runtime consumers do not acquire this dependency.

The pinned dependency stack has passed hosted Linux x86-64 GCC/Clang, Linux
arm64 and macOS builds, including all conformance profiles and installed
consumers. Separate Homebrew builds on arm64 macOS use Protobuf 36.1 and
OpenSSL 4.1. Windows MSVC also passes the full suite and installed consumers.
See [HOSTED_VERIFICATION.md](HOSTED_VERIFICATION.md) for
exact revisions and results, rather than inferring support from a manifest.

The Linux baseline requires GCC 14 or Clang 18. GCC 13.3 hits an internal compiler
error while compiling coroutine-heavy integration tests; GCC 14.2 compiles the
same tests. No tests are disabled to accommodate that compiler failure.
`ci/Dockerfile`, `ci/triplets/x64-linux.cmake`, and `ci/run-linux.sh` record the
container, release-only dependency triplet and local reproduction commands.

The manifest follows Microsoft's [baseline versioning](https://learn.microsoft.com/en-us/vcpkg/users/versioning)
model. The checked-out registry also fixes transitive port definitions and
build helpers. Upgrade it deliberately, rebuild generated protobuf code, and
rerun the full official matrix. The protobuf compiler and C++ runtime must match;
there is no stable binary ABI promise for this library.

Example setup (a vcpkg checkout can live outside the source tree):

```sh
git clone https://github.com/microsoft/vcpkg.git build/vcpkg
git -C build/vcpkg checkout 3d22b64585ca026e881728fd87e541c23a611a43
./build/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake -S . -B build-vcpkg -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCONNECTCPP_BUILD_CONFORMANCE=ON
cmake --build build-vcpkg
ctest --test-dir build-vcpkg --output-on-failure
```

Windows uses `bootstrap-vcpkg.bat`. Run from a compiler-enabled developer shell.
The default manifest feature includes GoogleTest. Runtime-only consumers can set
`VCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON`, `BUILD_TESTING=OFF`, and
`CONNECTCPP_BUILD_GENERATOR=OFF` together. Compiler/tool versions and target
triplets must also be recorded for reproducible binaries; a dependency baseline
alone does not make builds bit-for-bit identical.

Go dependencies are locked by `go.mod`/`go.sum`. The official conformance source
and fetched sanitizer GoogleTest source have independent archive hashes in
`cmake/Conformance.cmake` and `cmake/GoogleTest.cmake`. The protoc plugin's Go
version requirement is recorded in `go.mod`.
