cmake_minimum_required(VERSION 3.25)
project(connectcpp_lite_consumer LANGUAGES CXX)
find_package(connectcpp CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
# Protoc is a build tool; the executable links only the Lite runtime.
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/message.pb.cc" "${CMAKE_CURRENT_BINARY_DIR}/message.pb.h"
  COMMAND protobuf::protoc --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
    --cpp_out=${CMAKE_CURRENT_BINARY_DIR} message.proto
  DEPENDS message.proto VERBATIM)
add_executable(lite main.cc "${CMAKE_CURRENT_BINARY_DIR}/message.pb.cc")
target_include_directories(lite PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(lite PRIVATE connectcpp::connectcpp)
