#!/usr/bin/env bash
set -euo pipefail
: "${VCPKG_ROOT:?Set VCPKG_ROOT to the pinned vcpkg checkout}"
export CC=gcc-14 CXX=g++-14 VCPKG_MAX_CONCURRENCY=4
mkdir -p "$HOME"
overlay="${1:-$PWD/build/vcpkg-package-overlay}"
installed="$PWD/build/vcpkg-package-installed"
cmake -DOUTPUT_ROOT="$overlay" -P packaging/vcpkg/prepare.cmake
"$VCPKG_ROOT/vcpkg" install --x-manifest-root="$overlay/manifest" \
  --triplet=x64-linux --host-triplet=x64-linux \
  --overlay-ports="$overlay/connectcpp" --overlay-triplets="$PWD/ci/triplets" \
  --x-install-root="$installed"
for example in consumer callbacks headers; do
  cmake -S "examples/$example" -B "build-vcpkg-$example" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR="$installed" \
    -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_HOST_TRIPLET=x64-linux \
    -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release
  cmake --build "build-vcpkg-$example" --parallel 4
  if [[ "$example" != headers ]]; then "./build-vcpkg-$example/$example"; fi
done
# Reconfigure the task-local manifest to select the Lite variant. Keep the full
# package evidence above and use a separate installed tree and consumer build.
manifest="$overlay/manifest/vcpkg.json"
cmake -DMANIFEST="$manifest" -P packaging/vcpkg/enable-lite.cmake
installed="$PWD/build/vcpkg-lite-installed"
"$VCPKG_ROOT/vcpkg" install --x-manifest-root="$overlay/manifest" \
  --triplet=x64-linux --host-triplet=x64-linux \
  --overlay-ports="$overlay/connectcpp" --overlay-triplets="$PWD/ci/triplets" \
  --x-install-root="$installed"
cmake -S examples/lite -B build-vcpkg-lite -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR="$installed" \
  -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_HOST_TRIPLET=x64-linux \
  -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release
cmake --build build-vcpkg-lite --parallel 4
./build-vcpkg-lite/lite
if nm -C build-vcpkg-lite/lite | grep -E 'google::protobuf::(DescriptorPool|Reflection|Message::)'; then
  echo 'Full Protobuf symbols in Lite consumer' >&2
  exit 1
fi
