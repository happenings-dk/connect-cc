find_program(CONNECTCPP_GO go REQUIRED)
set(CONNECTCPP_PLUGIN "${CMAKE_CURRENT_BINARY_DIR}/bin/protoc-gen-connect-cpp${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(OUTPUT "${CONNECTCPP_PLUGIN}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/bin"
  COMMAND "${CONNECTCPP_GO}" build -trimpath -o "${CONNECTCPP_PLUGIN}" ./cmd/protoc-gen-connect-cpp
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  DEPENDS cmd/protoc-gen-connect-cpp/main.go internal/codegen/generate.go go.mod go.sum
  VERBATIM)
add_custom_target(connectcpp_codegen ALL DEPENDS "${CONNECTCPP_PLUGIN}")
install(PROGRAMS "${CONNECTCPP_PLUGIN}" DESTINATION ${CMAKE_INSTALL_BINDIR})
set(CONNECTCPP_CLI "${CMAKE_CURRENT_BINARY_DIR}/bin/connectcpp${CMAKE_EXECUTABLE_SUFFIX}")
file(GLOB_RECURSE connectcpp_tool_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/internal/tool/*" "${CMAKE_CURRENT_SOURCE_DIR}/cmd/connectcpp/*")
add_custom_command(OUTPUT "${CONNECTCPP_CLI}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/bin"
  COMMAND "${CONNECTCPP_GO}" build -trimpath -o "${CONNECTCPP_CLI}" ./cmd/connectcpp
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  DEPENDS ${connectcpp_tool_sources} go.mod go.sum
  VERBATIM)
add_custom_target(connectcpp_cli ALL DEPENDS "${CONNECTCPP_CLI}")
install(PROGRAMS "${CONNECTCPP_CLI}" DESTINATION ${CMAKE_INSTALL_BINDIR})
if(BUILD_TESTING AND NOT CONNECTCPP_PROTOBUF_LITE)
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
  add_custom_command(
    OUTPUT "${generated}/test/v1/echo.connect.cc" "${generated}/test/v1/echo.connect.h"
           "${generated}/test/v1/echo.pb.cc" "${generated}/test/v1/echo.pb.h"
           "${generated}/shared/v1/types.pb.cc" "${generated}/shared/v1/types.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${generated}"
    COMMAND protobuf::protoc --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/tests/proto
      --cpp_out=${generated} --connect-cpp_out=${generated}
      --plugin=protoc-gen-connect-cpp=${CONNECTCPP_PLUGIN}
      test/v1/echo.proto shared/v1/types.proto
    DEPENDS "${CONNECTCPP_PLUGIN}" tests/proto/test/v1/echo.proto tests/proto/shared/v1/types.proto
    VERBATIM)
  target_sources(connectcpp_test PRIVATE tests/generated_test.cc
    "${generated}/test/v1/echo.connect.cc" "${generated}/test/v1/echo.pb.cc"
    "${generated}/shared/v1/types.pb.cc")
  target_include_directories(connectcpp_test PRIVATE "${generated}")
  add_test(NAME Go.Generator COMMAND "${CONNECTCPP_GO}" test ./...)
  set_tests_properties(Go.Generator PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120)
endif()
if(BUILD_TESTING AND NOT CONNECTCPP_PROTOBUF_LITE)
  add_custom_command(
    OUTPUT "${generated}/interop/v1/service.connect.cc" "${generated}/interop/v1/service.connect.h"
           "${generated}/interop/v1/service.pb.cc" "${generated}/interop/v1/service.pb.h"
    COMMAND protobuf::protoc --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/tests/proto
      --cpp_out=${generated} --connect-cpp_out=${generated}
      --plugin=protoc-gen-connect-cpp=${CONNECTCPP_PLUGIN} interop/v1/service.proto
    DEPENDS "${CONNECTCPP_PLUGIN}" tests/proto/interop/v1/service.proto
    VERBATIM)
  add_executable(connectcpp_interop tests/interop_main.cc
    "${generated}/interop/v1/service.connect.cc" "${generated}/interop/v1/service.pb.cc")
  target_include_directories(connectcpp_interop PRIVATE "${generated}")
  target_link_libraries(connectcpp_interop PRIVATE connectcpp)
  # Compile the race-enabled Go helper during the build. A cold Go cache must
  # not consume the runtime timeout reserved for cross-language RPC checks.
  set(interop_go_test "${CMAKE_CURRENT_BINARY_DIR}/bin/connectcpp_interop_go_test${CMAKE_EXECUTABLE_SUFFIX}")
  add_custom_command(OUTPUT "${interop_go_test}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/bin"
    COMMAND "${CONNECTCPP_GO}" test -c -race -o "${interop_go_test}" ./internal/interop
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS internal/interop/interop_test.go go.mod go.sum
    VERBATIM)
  add_custom_target(connectcpp_interop_go_test ALL DEPENDS "${interop_go_test}")
  add_test(NAME Interop.Go COMMAND ${CMAKE_COMMAND} -E env
    CONNECTCPP_INTEROP_BINARY=$<TARGET_FILE:connectcpp_interop>
    "${interop_go_test}" -test.v)
  set_tests_properties(Interop.Go PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 90)
endif()
if(BUILD_TESTING AND NOT CONNECTCPP_PROTOBUF_LITE)
  add_custom_command(
    OUTPUT "${generated}/test/v1/edition.connect.cc" "${generated}/test/v1/edition.connect.h"
           "${generated}/test/v1/edition.pb.cc" "${generated}/test/v1/edition.pb.h"
           "${generated}/shared/v1/edition.pb.cc" "${generated}/shared/v1/edition.pb.h"
    COMMAND protobuf::protoc --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/tests/proto
      --cpp_out=${generated} --connect-cpp_out=${generated}
      --plugin=protoc-gen-connect-cpp=${CONNECTCPP_PLUGIN}
      test/v1/edition.proto shared/v1/edition.proto
    DEPENDS "${CONNECTCPP_PLUGIN}" tests/proto/test/v1/edition.proto tests/proto/shared/v1/edition.proto
    VERBATIM)
  target_sources(connectcpp_test PRIVATE tests/editions_test.cc
    "${generated}/test/v1/edition.connect.cc" "${generated}/test/v1/edition.pb.cc"
    "${generated}/shared/v1/edition.pb.cc")
endif()
