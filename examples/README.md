# Examples

These examples consume the installed CMake package, so they also exercise its
exported dependencies without relying on source-tree include paths.

| Example | Purpose |
| --- | --- |
| [callbacks](callbacks/main.cc) | Complete local client/server conversation over HTTP/2: all four RPC shapes, callback clients, coroutine handlers, interceptors and joined shutdown |
| [consumer](consumer/main.cc) | Small integration checks for the installed runtime, codecs, routing and connection APIs |
| [lite](lite/main.cc) | Standalone Lite-only installed consumer with generated messages and binary error details |
| [native-trust](native-trust/main.cc) | Managed HTTPS exchange using system trust; optional online probe with no OS store changes |
| [headers](headers/CMakeLists.txt) | Compile each public header independently to catch missing exported dependencies |

For generated clients and handlers, start with the
[getting started guide](../docs/GETTING_STARTED.md). The
[generated-binding tests](../tests/generated_test.cc) additionally exercise
imported and nested messages, naming collisions and all RPC shapes.

## Build and run

First build and install the library as described in
[Getting started](../docs/GETTING_STARTED.md). Then, from the repository root:

```sh
cmake -S examples/callbacks -B build-callbacks -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/install" -DCMAKE_BUILD_TYPE=Release
cmake --build build-callbacks
./build-callbacks/callbacks
```

A successful callback example exits with status zero. It creates a loopback
listener on an ephemeral port, runs the conversation, checks completion on both
endpoints, and closes its connections. It does not contact an external service.

When using vcpkg, pass the same `CMAKE_TOOLCHAIN_FILE`, target/host triplets and
`VCPKG_INSTALLED_DIR` as the library build, plus
`-DVCPKG_MANIFEST_INSTALL=OFF`. Use the same compiler and sanitizer settings as
the installed library. On Windows, run `build-callbacks/callbacks.exe` from a
compiler-enabled environment. See [Packaging](../docs/PACKAGING.md) for complete
package-manager consumer commands.

The consumer example follows the same commands with `consumer` in place of
`callbacks`. The headers example only needs to compile; it has no executable.

The Lite example needs an installation built with `CONNECTCPP_PROTOBUF_LITE=ON`.
Build it against that installation in a separate directory. The native-trust
example accepts an optional HTTPS base URL (default `https://connectrpc.com`)
and requires outbound network access.
