#!/usr/bin/env bash
set -euo pipefail
mode="${1:?Expected address, thread or fuzz}"
: "${VCPKG_ROOT:?Set VCPKG_ROOT to the pinned vcpkg checkout}"
mkdir -p "$HOME"
export VCPKG_MAX_CONCURRENCY=4
export GOCACHE="$PWD/build/go-cache"
export GOPATH="$PWD/build/.go"
flags=(-DCMAKE_BUILD_TYPE=RelWithDebInfo)
triplet=x64-linux-asan
build_dir="build-linux-$mode"
installed_dir="$PWD/build/vcpkg-address-installed"
case "$mode" in
  address) flags+=(-DCONNECTCPP_SANITIZE=ON -DCONNECTCPP_BUILD_CONFORMANCE=ON) ;;
  thread)
    triplet=x64-linux-tsan
    build_dir=build-linux-thread-instrumented
    installed_dir="$PWD/build/vcpkg-thread-installed"
    flags+=(-DCONNECTCPP_THREAD_SANITIZE=ON -DCONNECTCPP_BUILD_CONFORMANCE=ON)
    ;;
  fuzz) flags+=(-DCONNECTCPP_BUILD_FUZZERS=ON -DCONNECTCPP_SANITIZE=ON -DBUILD_TESTING=OFF -DCONNECTCPP_BUILD_GENERATOR=OFF) ;;
  *) echo 'Expected address, thread or fuzz' >&2; exit 2 ;;
esac
cmake -S . -B "$build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/ci/triplets" \
  -DVCPKG_TARGET_TRIPLET="$triplet" -DVCPKG_HOST_TRIPLET=x64-linux \
  -DVCPKG_INSTALLED_DIR="$installed_dir" \
  -DVCPKG_INSTALL_OPTIONS=--clean-buildtrees-after-build \
  -DCMAKE_CXX_COMPILER=clang++-18 "${flags[@]}"
cmake --build "$build_dir" --parallel 2
if [[ "$mode" == fuzz ]]; then
  mkdir -p build-linux-fuzz/corpus build-linux-fuzz/artifacts
  cp fuzz/corpus/* build-linux-fuzz/corpus/
  build-linux-fuzz/connectcpp_protocol_fuzz build-linux-fuzz/corpus \
    -max_total_time=120 -max_len=4096 -timeout=10 -rss_limit_mb=1024 \
    -artifact_prefix=build-linux-fuzz/artifacts/
else
  ctest --test-dir "$build_dir" --parallel 3 --output-on-failure
fi
