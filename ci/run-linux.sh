#!/usr/bin/env bash
set -euo pipefail
compiler="${1:-gcc}"
case "$compiler" in
  gcc|gcc14) c_compiler=gcc-14; cxx_compiler=g++-14 ;;
  clang) c_compiler=clang-18; cxx_compiler=clang++-18 ;;
  *) echo 'Expected gcc or clang' >&2; exit 2 ;;
esac
: "${VCPKG_ROOT:?Set VCPKG_ROOT to the pinned vcpkg checkout}"
mkdir -p "$HOME" build/logs
export VCPKG_MAX_CONCURRENCY=4
export GOCACHE="$PWD/build/go-cache"
export GOPATH="$PWD/build/.go"
"$cxx_compiler" --version
go version
cmake --version
git -C "$VCPKG_ROOT" rev-parse HEAD
cmake -S . -B "build-linux-$compiler" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/ci/triplets" \
  -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_HOST_TRIPLET=x64-linux \
  -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg-installed" \
  -DVCPKG_INSTALL_OPTIONS=--clean-buildtrees-after-build \
  -DCMAKE_C_COMPILER="$c_compiler" -DCMAKE_CXX_COMPILER="$cxx_compiler" \
  -DCMAKE_BUILD_TYPE=Release -DCONNECTCPP_BUILD_CONFORMANCE=ON
cmake --build "build-linux-$compiler" --parallel 4
ctest --test-dir "build-linux-$compiler" --parallel 4 --output-on-failure
cmake --install "build-linux-$compiler" --prefix "$PWD/install-linux-$compiler"
for example in consumer callbacks headers; do
  cmake -S "examples/$example" -B "build-linux-$compiler-$example" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_MANIFEST_INSTALL=OFF \
    -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_HOST_TRIPLET=x64-linux \
    -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg-installed" \
    -DCMAKE_CXX_COMPILER="$cxx_compiler" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PWD/install-linux-$compiler"
  cmake --build "build-linux-$compiler-$example" --parallel 4
  if [[ "$example" != headers ]]; then "./build-linux-$compiler-$example/$example"; fi
done
