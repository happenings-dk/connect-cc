set(CMAKE_C_COMPILER clang-18 CACHE STRING "C compiler")
set(CMAKE_CXX_COMPILER clang++-18 CACHE STRING "C++ compiler")
# Chainloading replaces vcpkg's platform toolchain. Include it explicitly so
# the triplet's sanitizer/PIC flags reach dependency builds as well.
if(DEFINED Z_VCPKG_ROOT_DIR)
  include("${Z_VCPKG_ROOT_DIR}/scripts/toolchains/linux.cmake")
else()
  get_filename_component(connectcpp_vcpkg_buildsystems "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
  include("${connectcpp_vcpkg_buildsystems}/../toolchains/linux.cmake")
  unset(connectcpp_vcpkg_buildsystems)
endif()
