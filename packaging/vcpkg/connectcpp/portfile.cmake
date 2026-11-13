# prepare.cmake places a source snapshot inside this overlay port, so vcpkg's
# port hash changes whenever the packaged implementation changes.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
set(SOURCE_PATH "${CURRENT_PORT_DIR}/source")
if(NOT EXISTS "${SOURCE_PATH}/CMakeLists.txt")
  message(FATAL_ERROR "Prepare the self-contained overlay with packaging/vcpkg/prepare.cmake")
endif()
vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    -DBUILD_TESTING=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DCONNECTCPP_BUILD_GENERATOR=OFF
    -DCONNECTCPP_BUILD_CONFORMANCE=OFF)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/connectcpp)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
