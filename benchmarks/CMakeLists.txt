find_package(gRPC CONFIG REQUIRED)
set(benchmark_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
get_target_property(protobuf_includes protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
set(benchmark_proto_paths "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}")
foreach(path IN LISTS protobuf_includes)
  if(EXISTS "${path}/google/protobuf/wrappers.proto")
    list(APPEND benchmark_proto_paths "--proto_path=${path}")
  endif()
endforeach()
add_custom_command(
  OUTPUT "${benchmark_generated}/echo.pb.cc" "${benchmark_generated}/echo.pb.h"
         "${benchmark_generated}/echo.grpc.pb.cc" "${benchmark_generated}/echo.grpc.pb.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${benchmark_generated}"
  COMMAND protobuf::protoc ${benchmark_proto_paths}
    --cpp_out=${benchmark_generated} --grpc_out=${benchmark_generated}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin> echo.proto
  DEPENDS echo.proto protobuf::protoc gRPC::grpc_cpp_plugin
  VERBATIM)
add_executable(connectcpp_benchmark main.cc
  "${benchmark_generated}/echo.pb.cc" "${benchmark_generated}/echo.grpc.pb.cc")
target_include_directories(connectcpp_benchmark PRIVATE "${benchmark_generated}")
target_link_libraries(connectcpp_benchmark PRIVATE connectcpp::connectcpp gRPC::grpc++)
if(NOT MSVC)
  target_compile_options(connectcpp_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()
