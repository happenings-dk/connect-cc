# Protobuf Lite

The runtime supports Google's `MessageLite` classes and a build that links only
`libprotobuf-lite`. Use Lite when binary protobuf messages are sufficient and
reflection is unnecessary. Connect protocol errors and end-stream JSON still
work: their bounded JSON implementation does not depend on Protobuf reflection.
All three RPC protocols, all RPC shapes, gzip, binary typed error details and
managed connections remain available.

## Build

```sh
cmake -S . -B build-lite -G Ninja \
  -DCONNECTCPP_PROTOBUF_LITE=ON -DCONNECTCPP_BUILD_GENERATOR=ON
cmake --build build-lite --parallel 3
ctest --test-dir build-lite --output-on-failure
cmake --install build-lite --prefix "$PWD/install-lite"
```

The installed `connectcpp::connectcpp` target exports both its Lite dependency
and `CONNECTCPP_PROTOBUF_LITE=1`. Consume that target so every translation unit
uses the same public codec declarations. Keep full and Lite installations in
separate prefixes. Match your `protoc` version to the linked runtime.
The official conformance adapter needs protobuf JSON and is built separately
with the full runtime; enabling it in a Lite build produces a configuration error.

Generate message classes with:

```protobuf
syntax = "proto3";
package example;
option optimize_for = LITE_RUNTIME;
message Message { string value = 1; }
```

The service generator accepts Lite schemas. It continues to generate the same
typed clients and abstract handlers; binary encoding uses `MessageLite` methods.
Full-runtime builds also accept Lite messages alongside full message classes.

## JSON capability

Lite message classes do not contain Google's descriptors/reflection, so
application-message JSON encoding and decoding return `unimplemented`. This
also applies to JSON Connect GET requests. Binary GET remains supported.
Use full-runtime generated classes and the default runtime build when you need
protobuf JSON. No dynamic cast or hidden full-runtime fallback is performed.

Protocol JSON is a separate concern. Error and end-stream messages support
Unicode, escaping, typed details and metadata. Reads reject malformed UTF-8,
invalid numbers, duplicate object keys, nesting beyond 64 levels and documents
with more than 65,536 values. Existing message/metadata byte limits still apply.
These limits apply to protocol JSON in both builds.

## Packages and evidence

- CMake: `-DCONNECTCPP_PROTOBUF_LITE=ON`.
- Local vcpkg overlay: select `connectcpp[lite]`.
- Conan: `-o 'connectcpp/*:protobuf_lite=True'` creates a distinct package ID.
- [Installed consumer](../examples/lite): generates a Lite message, checks
  binary serialization and error details, and links the installed target.

CI builds full and Lite variants separately. Lite tests include real TCP/TLS
RPCs and generated bindings. Link/symbol inspection accompanies installed
consumer checks; merely compiling Lite message headers is not evidence of a
Lite-only runtime.
