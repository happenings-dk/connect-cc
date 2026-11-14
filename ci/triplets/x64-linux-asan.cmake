set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/clang18.cmake")
# Protobuf's container annotations and message globals must match the caller.
set(VCPKG_C_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
set(VCPKG_CXX_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
set(VCPKG_LINKER_FLAGS "-fsanitize=address,undefined")
# Match OpenSSL's upstream Clang UBSan configuration for its erased C callbacks.
# Keep function-type checks enabled for connect-cc and every other dependency.
# See docs/SANITIZERS.md for the upstream issue and exact configuration source.
if(PORT STREQUAL "openssl")
  string(APPEND VCPKG_C_FLAGS " -DPEDANTIC -fno-sanitize-recover=all -fno-sanitize=function")
endif()
