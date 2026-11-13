#!/usr/bin/env bash
set -euo pipefail
: "${VCPKG_ROOT:?Set VCPKG_ROOT to the pinned vcpkg checkout}"
export CC=gcc-14 CXX=g++-14 VCPKG_MAX_CONCURRENCY=4
output="${1:-$PWD/build/benchmark-results}"
cmake -S . -B build-benchmarks -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/ci/triplets" \
  -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_HOST_TRIPLET=x64-linux \
  -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg-benchmark-installed" \
  -DVCPKG_MANIFEST_FEATURES=benchmarks \
  -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=OFF \
  -DVCPKG_INSTALL_OPTIONS=--clean-buildtrees-after-build \
  -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCONNECTCPP_BUILD_GENERATOR=OFF -DCONNECTCPP_BUILD_CONFORMANCE=OFF \
  -DCONNECTCPP_SANITIZE=OFF -DCONNECTCPP_THREAD_SANITIZE=OFF \
  -DCONNECTCPP_BUILD_BENCHMARKS=ON
cmake --build build-benchmarks --parallel 4 --target connectcpp_benchmark
benchmarks/run.sh "$PWD/build-benchmarks/benchmarks/connectcpp_benchmark" "$output"
cp build-benchmarks/CMakeCache.txt "$output/CMakeCache.txt"
cp build/vcpkg-benchmark-installed/vcpkg/status "$output/dependencies.txt"
{
  g++-14 --version
  cmake --version
  git -C "$VCPKG_ROOT" rev-parse HEAD
} > "$output/toolchain.txt"
find benchmarks include src proto -type f -print0 | sort -z | xargs -0 sha256sum > "$output/source-sha256.txt"
sha256sum CMakeLists.txt vcpkg.json ci/run-benchmarks.sh ci/triplets/x64-linux.cmake >> "$output/source-sha256.txt"
sha256sum build-benchmarks/benchmarks/connectcpp_benchmark > "$output/binary-sha256.txt"
