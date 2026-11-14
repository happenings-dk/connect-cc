#!/usr/bin/env bash
set -euo pipefail
export CONAN_HOME="${CONAN_HOME:-$PWD/build/.conan-home}"
export GOCACHE="$PWD/build/go-cache" GOPATH="$PWD/build/.go"
profile="$PWD/packaging/conan/linux-gcc14"
lockfile="$PWD/packaging/conan/conan.lock"
if [[ ! -f "$lockfile" ]]; then
  conan lock create . -pr:h "$profile" -pr:b "$profile" \
    --lockfile-out="$lockfile"
fi
conan create . -pr:h "$profile" -pr:b "$profile" \
  --lockfile="$lockfile" --build=missing
conan install . -pr:h "$profile" -pr:b "$profile" \
  --lockfile="$lockfile" --build=missing --output-folder=build/conan-verify
generators="$PWD/build/conan-verify/build/Release/generators"
# Conan build tools, including the matching native protoc, are isolated here.
source "$generators/conanbuild.sh"
cmake -S . -B build-conan-matrix -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$generators/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCONNECTCPP_FETCH_GTEST=ON -DCONNECTCPP_BUILD_GENERATOR=ON \
  -DCONNECTCPP_BUILD_CONFORMANCE=ON
cmake --build build-conan-matrix --parallel 4
ctest --test-dir build-conan-matrix --parallel 4 --output-on-failure
conan install packaging/conan/consumer -pr:h "$profile" -pr:b "$profile" \
  --build=missing --output-folder=build/conan-consumer-deps
for example in consumer callbacks headers; do
  cmake -S "examples/$example" -B "build-conan-$example" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/build/conan-consumer-deps/conan_toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build "build-conan-$example" --parallel 4
  if [[ "$example" != headers ]]; then "./build-conan-$example/$example"; fi
done
# The option is a separate package ID; its independent consumer generates a
# Lite message and exercises binary status details without full Protobuf.
conan create . -pr:h "$profile" -pr:b "$profile" \
  --lockfile="$lockfile" --build=missing -o 'connectcpp/*:protobuf_lite=True'
