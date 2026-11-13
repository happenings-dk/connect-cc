# The suite and its protobuf definitions must always come from the same revision.
set(CONNECTCPP_CONFORMANCE_REVISION 73e45c4dda9c133fab1538d6d6c6d66c7deed844)
include(FetchContent)
FetchContent_Declare(connectcpp_conformance_source
  GIT_REPOSITORY https://github.com/connectrpc/conformance.git
  GIT_TAG ${CONNECTCPP_CONFORMANCE_REVISION}
  SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/conformance")
FetchContent_MakeAvailable(connectcpp_conformance_source)
find_program(CONNECTCPP_GO go REQUIRED)
set(conformance_generated "${CMAKE_CURRENT_BINARY_DIR}/conformance-generated")
set(conformance_sources)
foreach(name config service client_compat server_compat)
  set(proto "connectrpc/conformance/v1/${name}")
  list(APPEND conformance_sources "${conformance_generated}/${proto}.pb.cc")
  add_custom_command(OUTPUT "${conformance_generated}/${proto}.pb.cc" "${conformance_generated}/${proto}.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${conformance_generated}"
    COMMAND protobuf::protoc --proto_path=${connectcpp_conformance_source_SOURCE_DIR}/proto
      --cpp_out=${conformance_generated} ${proto}.proto
    DEPENDS "${connectcpp_conformance_source_SOURCE_DIR}/proto/${proto}.proto"
    VERBATIM)
endforeach()
add_library(connectcpp_conformance_proto STATIC ${conformance_sources})
target_include_directories(connectcpp_conformance_proto PUBLIC "${conformance_generated}")
target_link_libraries(connectcpp_conformance_proto PUBLIC connectcpp)
add_executable(connectcpp_conformance conformance/main.cc conformance/client.cc conformance/streaming_client.cc conformance/server.cc conformance/streaming_server.cc conformance/io.cc)
target_include_directories(connectcpp_conformance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(connectcpp_conformance PRIVATE connectcpp_conformance_proto)
set(conformance_runner "${CMAKE_CURRENT_BINARY_DIR}/bin/connectconformance${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(OUTPUT "${conformance_runner}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/bin"
  COMMAND "${CONNECTCPP_GO}" build -trimpath -o "${conformance_runner}" ./cmd/connectconformance
  WORKING_DIRECTORY "${connectcpp_conformance_source_SOURCE_DIR}"
  DEPENDS "${connectcpp_conformance_source_SOURCE_DIR}/go.mod" "${connectcpp_conformance_source_SOURCE_DIR}/go.sum"
  VERBATIM)
add_custom_target(connectcpp_conformance_runner ALL DEPENDS "${conformance_runner}")
if(BUILD_TESTING)
  foreach(mode client server)
    add_test(NAME Conformance.http1_${mode} COMMAND "${conformance_runner}"
      --mode ${mode} --conf "${CMAKE_CURRENT_SOURCE_DIR}/conformance/http1.yaml"
      --max-servers 2 -- $<TARGET_FILE:connectcpp_conformance> ${mode})
    set_tests_properties(Conformance.http1_${mode} PROPERTIES TIMEOUT 300 LABELS conformance)
  endforeach()
  foreach(mode client server)
    add_test(NAME Conformance.${mode} COMMAND "${conformance_runner}"
      --mode ${mode} --conf "${CMAKE_CURRENT_SOURCE_DIR}/conformance/unary.yaml"
      --max-servers 2 -- $<TARGET_FILE:connectcpp_conformance> ${mode})
    set_tests_properties(Conformance.${mode} PROPERTIES TIMEOUT 300 LABELS conformance)
  endforeach()
  foreach(mode client server)
    add_test(NAME Conformance.streaming_${mode} COMMAND "${conformance_runner}"
      --mode ${mode} --conf "${CMAKE_CURRENT_SOURCE_DIR}/conformance/streaming.yaml"
      --max-servers 2 -- $<TARGET_FILE:connectcpp_conformance> ${mode})
    set_tests_properties(Conformance.streaming_${mode} PROPERTIES TIMEOUT 300 LABELS conformance)
  endforeach()
  foreach(mode client server)
    add_test(NAME Conformance.grpc_${mode} COMMAND "${conformance_runner}"
      --mode ${mode} --conf "${CMAKE_CURRENT_SOURCE_DIR}/conformance/grpc.yaml"
      --max-servers 2 -- $<TARGET_FILE:connectcpp_conformance> ${mode})
    set_tests_properties(Conformance.grpc_${mode} PROPERTIES TIMEOUT 300 LABELS conformance)
  endforeach()
  foreach(mode client server)
    add_test(NAME Conformance.grpc_web_${mode} COMMAND "${conformance_runner}"
      --mode ${mode} --conf "${CMAKE_CURRENT_SOURCE_DIR}/conformance/grpc_web.yaml"
      --max-servers 2 -- $<TARGET_FILE:connectcpp_conformance> ${mode})
    set_tests_properties(Conformance.grpc_web_${mode} PROPERTIES TIMEOUT 300 LABELS conformance)
  endforeach()
endif()
