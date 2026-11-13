# Instrument the test framework with the same compiler/standard-library mode as
# the tests. Mixing prebuilt GoogleTest with ASan libc++ container annotations
# can fail during test discovery, before any library code executes.
set(connectcpp_instrument_tests OFF)
if(CONNECTCPP_SANITIZE OR CONNECTCPP_THREAD_SANITIZE)
  set(connectcpp_instrument_tests ON)
endif()
option(CONNECTCPP_FETCH_GTEST "Build pinned GoogleTest from source" ${connectcpp_instrument_tests})
if(CONNECTCPP_FETCH_GTEST)
  include(FetchContent)
  set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "Build GoogleMock" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "Use shared Windows runtime" FORCE)
  FetchContent_Declare(connectcpp_googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(connectcpp_googletest)
  foreach(test_target gtest gtest_main)
    target_compile_features(${test_target} PUBLIC cxx_std_20)
    if(CONNECTCPP_SANITIZE AND NOT MSVC)
      target_compile_options(${test_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${test_target} PRIVATE -fsanitize=address,undefined)
    elseif(CONNECTCPP_THREAD_SANITIZE)
      target_compile_options(${test_target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
      target_link_options(${test_target} PRIVATE -fsanitize=thread)
    endif()
  endforeach()
else()
  find_package(GTest CONFIG REQUIRED)
endif()
