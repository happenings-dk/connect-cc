# Local package recipes

The runtime is Apache-2.0 and both recipes include `LICENSE`. Packages are
experimental static libraries, with no stable binary ABI promise. Rebuild
applications when dependency versions or compiler settings change. The recipes
and verification scripts do not publish repositories, releases or registry
entries.

## vcpkg

Use the registry revision recorded in `vcpkg.json` and DEPENDENCIES.md. Prepare
a fresh, self-contained source snapshot:

```sh
cmake -DOUTPUT_ROOT="$PWD/build/my-overlay" -P packaging/vcpkg/prepare.cmake
"$VCPKG_ROOT/vcpkg" install \
  --x-manifest-root="$PWD/build/my-overlay/manifest" \
  --overlay-ports="$PWD/build/my-overlay/connectcpp" \
  --overlay-triplets="$PWD/ci/triplets" \
  --triplet=x64-linux --host-triplet=x64-linux \
  --x-install-root="$PWD/build/my-packages"
cmake -S examples/consumer -B build/package-consumer -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_MANIFEST_INSTALL=OFF \
  -DVCPKG_INSTALLED_DIR="$PWD/build/my-packages" \
  -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_HOST_TRIPLET=x64-linux \
  -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release
cmake --build build/package-consumer
./build/package-consumer/consumer
```

The commands above describe the verified Linux profile. Set `CC=gcc-14` and
`CXX=g++-14` when building its dependencies too. Other triplets require their own
compiler environments and verification; a portable recipe is not proof of a
successful Windows build.

The snapshot includes implementation files inside the port, so its vcpkg hash
changes with the source. Manifest mode then rebuilds an installed package whose
ABI key changed. This avoids relying on an unpublished Git URL or silently
reusing a stale binary from a mutable source directory. Each preparation uses a
new output directory and refuses to overwrite an existing port snapshot.
`ci/run-vcpkg-package.sh` performs installation and tests the coroutine/callback
examples and all public headers independently of the source tree.

## Conan 2

The root `conanfile.py` requires Conan 2.34.0. Its runtime versions are pinned;
`packaging/conan/conan.lock` additionally fixes dependency recipe revisions and
build-tool requirements. Keep Conan caches outside the source tree or in a
hidden directory so `go test ./...` does not traverse dependencies' Go sources.

```sh
python3 -m venv build/conan-venv
build/conan-venv/bin/pip install conan==2.34.0
export PATH="$PWD/build/conan-venv/bin:$PATH"
export CONAN_HOME="$PWD/build/.conan-home"
conan create . \
  -pr:h packaging/conan/linux-gcc14 -pr:b packaging/conan/linux-gcc14 \
  --lockfile=packaging/conan/conan.lock --build=missing
```

`conan create` compiles and runs `test_package` against the package in Conan's
cache. Public protobuf, Asio and OpenSSL requirements propagate to consumers;
native `protoc` is a separate build requirement matching the target runtime's
version. The supplied profile uses GCC 14, C++20, Release and four build jobs.
Use a separately validated profile for another platform.

The standalone example can also consume the package through CMakeDeps:

```sh
conan install packaging/conan/consumer \
  -pr:h packaging/conan/linux-gcc14 -pr:b packaging/conan/linux-gcc14 \
  --output-folder=build/conan-consumer-deps --build=missing
cmake -S examples/consumer -B build/conan-consumer -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/conan-consumer-deps/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/conan-consumer
./build/conan-consumer/consumer
```

The generated CMake target has the same `connectcpp::connectcpp` name and
public C++20 requirement as the native install. `ci/run-conan-package.sh` also
builds the complete source test matrix against Conan's dependency stack, then
compiles and runs the separate examples and all public headers. The prepared
GitHub package workflow runs these scripts; hosted jobs have not been executed.

Conan Center currently provides nghttp2 1.68.1 and llhttp 9.3.0 in this recipe,
whereas the vcpkg baseline uses 1.71.0 and 9.4.3. The other runtime versions are
Protobuf 7.36.2, Asio 1.38.2, OpenSSL 3.6.5, Abseil 20260107.1 and zlib 1.4.
The HTTP/1 wrapper explicitly rejects empty `Transfer-Encoding` fields across
these parser versions. This was reproduced against llhttp 9.3.0 and fixed with
a fragmented client/server regression; newer llhttp rejects it internally too.

## Generator

The package recipes install the C++ runtime. Build the Go generator from the
same source snapshot, without adding Go to applications' runtime dependencies:

```sh
go build -trimpath -o build/bin/protoc-gen-connect-cpp ./cmd/protoc-gen-connect-cpp
```

Use that binary with protoc or Buf as shown in CODEGEN.md. Google C++ protobuf
generation must use the compiler version matching the linked protobuf runtime.
