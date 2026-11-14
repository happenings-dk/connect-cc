include(cmake/GoogleTest.cmake)
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT "${generated}/test/v1/lite.connect.cc" "${generated}/test/v1/lite.connect.h"
         "${generated}/test/v1/lite.pb.cc" "${generated}/test/v1/lite.pb.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${generated}"
  COMMAND protobuf::protoc --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/tests/proto
    --cpp_out=${generated} --connect-cpp_out=${generated}
    --plugin=protoc-gen-connect-cpp=${CONNECTCPP_PLUGIN} test/v1/lite.proto
  DEPENDS "${CONNECTCPP_PLUGIN}" tests/proto/test/v1/lite.proto VERBATIM)
add_executable(connectcpp_test tests/lite_test.cc tests/tls_shutdown_test.cc tests/managed_transport_test.cc tests/protocol_json_test.cc
  "${generated}/test/v1/lite.connect.cc" "${generated}/test/v1/lite.pb.cc")
target_include_directories(connectcpp_test PRIVATE "${generated}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(connectcpp_test PRIVATE connectcpp GTest::gtest_main)
include(cmake/TestCertificate.cmake)
include(GoogleTest)
gtest_discover_tests(connectcpp_test PROPERTIES TIMEOUT 15)
