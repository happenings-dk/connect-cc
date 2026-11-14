cmake_minimum_required(VERSION 3.25)
project(connectcpp_package_test LANGUAGES CXX)
find_package(connectcpp CONFIG REQUIRED)
get_target_property(definitions connectcpp::connectcpp INTERFACE_COMPILE_DEFINITIONS)
if("${definitions}" MATCHES "CONNECTCPP_PROTOBUF_LITE")
  find_package(Protobuf CONFIG REQUIRED)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/message.pb.cc" "${CMAKE_CURRENT_BINARY_DIR}/message.pb.h"
    COMMAND protobuf::protoc --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
      --cpp_out=${CMAKE_CURRENT_BINARY_DIR} message.proto
    DEPENDS message.proto VERBATIM)
  add_executable(consumer lite_main.cc "${CMAKE_CURRENT_BINARY_DIR}/message.pb.cc")
  target_include_directories(consumer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
else()
  add_executable(consumer main.cc)
endif()
target_link_libraries(consumer PRIVATE connectcpp::connectcpp)
