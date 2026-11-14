# Install the SDK and start another project

Connect C++ provides a compiled C++20 library, a Protobuf service generator,
and the `connectcpp` developer CLI. Install the SDK once into a user-owned
prefix, then generate and build independent projects without copying this repo.
The CLI remembers the dependency/toolchain paths used to build the SDK.

This is an independent, pre-release project. Distribution currently uses a
source checkout; GitHub access is required while the repository is private.
There is no public registry package or stable binary ABI promise. Pin a source
revision and dependency baseline in reproducible builds.

## Prerequisites

Install Go 1.26+, CMake 3.25+, Ninja, and a C++20 compiler. Dependencies are
Protobuf, Asio, Abseil, OpenSSL, nghttp2, llhttp, zlib, and pkg-config.
Go is needed for the CLI/generator build, not by the compiled application.
Use the same Protobuf compiler and runtime versions.

On macOS with Homebrew:

```sh
brew install go cmake ninja pkgconf protobuf asio abseil openssl@3 libnghttp2 llhttp
```

The setup command discovers Homebrew's prefix. For reproducibility across Linux,
macOS, and Windows, use the pinned vcpkg checkout described below. Linux's tested
baseline is GCC 14 or Clang 18. Windows uses MSVC in a developer terminal.

## Install once

From a Connect C++ checkout with dependencies available:

```sh
./install.sh
# Equivalent on every platform:
go run ./cmd/connectcpp setup --source .
```

PowerShell also supports `./install.ps1`. The default prefix is `~/.local`;
select another with `--prefix /path/to/sdk`. Add its `bin` directory to `PATH`.
On POSIX shells, for the current terminal:

```sh
export PATH="$HOME/.local/bin:$PATH"
connectcpp doctor
```

PowerShell, for the current terminal:

```powershell
$env:Path = "$HOME\.local\bin;$env:Path"
connectcpp doctor
```

Setup builds the runtime and tools, installs the CMake package and headers, and
records build dependency paths in `PREFIX/share/connectcpp/sdk.json`. It does not
edit shell startup files, install system packages, or fetch unpinned code. Run the
same bootstrap command from an updated checkout to upgrade; on Windows do this
with `go run` or `install.ps1` so the installed executable is not running while
it is replaced. Rebuild your applications after SDK or dependency upgrades.

For a pinned vcpkg installation, first clone and bootstrap the version in
[DEPENDENCIES.md](DEPENDENCIES.md), then pass its toolchain file to setup:

```sh
go run ./cmd/connectcpp setup --source . \
  --cmake-arg "-DCMAKE_TOOLCHAIN_FILE=$PWD/build/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

On Windows use the equivalent absolute Windows path and `bootstrap-vcpkg.bat`.
`--cmake-arg` is repeatable. Use it to set `VCPKG_TARGET_TRIPLET`,
`VCPKG_HOST_TRIPLET`, `VCPKG_INSTALLED_DIR`, `CMAKE_CXX_COMPILER`, or an explicit
`CMAKE_PREFIX_PATH`. These toolchain/dependency locations must remain available
for future application builds. Use a fresh `--build-dir` when changing ABI,
compiler, architecture, or runtime variant. Do not mix SDKs for incompatible
configurations in one prefix. The convenience CLI builds a native SDK; for
cross-compilation, supply host generators with your normal CMake toolchain flow.

## Create a project

```sh
connectcpp new hello
cd hello
connectcpp build
./build/greeter serve
```

In another terminal:

```sh
./build/greeter call http://127.0.0.1:8080 Ada
```

The result is `Hello, Ada`. Windows uses `build\greeter.exe`.
The build also runs a real TCP client/server round-trip test. The starter listens
on loopback over cleartext HTTP/1.1; Ctrl+C stops admission and drains connections.
Use verified TLS for remote services and HTTP/2 for bidirectional streaming.

Edit `proto/greet/v1/greet.proto` and the handler in `src/main.cc`, then rerun
`connectcpp build`. CMake regenerates messages and service bindings. The project
is ordinary CMake and can use another build frontend at any time.

`connectcpp build --prefix /path/to/sdk /path/to/project` selects another SDK.
`CONNECTCPP_ROOT` also selects a prefix; otherwise the CLI discovers metadata
relative to its own installed executable and finally tries `~/.local`.
`setup --config Debug --prefix PATH --build-dir PATH` creates a separate Debug
SDK. `build` defaults to the SDK's configuration; `build --config Debug` overrides
it, but the SDK and dependencies must provide compatible Debug libraries on
platforms that require them. `doctor` checks tools
and installed files; the build is the actual dependency/compile check.

## Existing CMake project

```cmake
find_package(connectcpp CONFIG REQUIRED)
connectcpp_generate(TARGET my_api
  PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/proto"
  SERVICES greet/v1/greet.proto
  MESSAGES shared/v1/types.proto)
add_executable(my_app main.cc)
target_link_libraries(my_app PRIVATE my_api)
```

Omit `MESSAGES` when no message-only schema needs generating. `SERVICES` schemas
produce `.pb.*` and `.connect.*`; `MESSAGES` schemas produce `.pb.*` only. Import
paths are relative to `PROTO_ROOT`. `IMPORT_DIRS` adds search roots; it does not
link imported message implementations automatically. List those messages or link
the library that owns them. Generated files live in the build tree and imported
schema changes trigger regeneration. The helper selects Lite message generation
when the installed SDK uses Lite; see [PROTOBUF_LITE.md](PROTOBUF_LITE.md).

For manual CMake use, set `CMAKE_PREFIX_PATH` to the SDK prefix and dependency
prefixes (or use the matching vcpkg/Conan toolchain). The generator is found in
the SDK's `bin`; override `CONNECTCPP_PROTOC_GEN` for a separate host generator.
The CLI's `sdk.json` records the equivalent dependency arguments for inspection.

The Go module path is `github.com/happenings-dk/connect-cc`. In environments with
repository access, the tools can also be installed with `go install` at a pinned
revision. This installs the tools only; `setup` still needs a source checkout and
C++ dependencies to build the SDK. Do not mistake `go install` for installing the
C++ runtime or a published binary package.

## Agent skill

```sh
connectcpp skill install
```

This installs `connect-cpp` into `~/.agents/skills/connect-cpp`. Agents that do not
discover that shared directory can use `--dir ~/.codex/skills/connect-cpp`, a
Claude skill directory, or a link to the shared copy. Installation refuses to
overwrite an existing skill. Review/merge local skill changes before upgrading.
Combine it with the general `connectrpc` and `google-cpp-style` skills.
