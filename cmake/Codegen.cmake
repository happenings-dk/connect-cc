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
if(BUILD_TESTING)
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
if(BUILD_TESTING)
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
  add_test(NAME Interop.Go COMMAND ${CMAKE_COMMAND} -E env
    CONNECTCPP_INTEROP_BINARY=$<TARGET_FILE:connectcpp_interop>
    "${CONNECTCPP_GO}" test -race -count=1 -v ./internal/interop)
  set_tests_properties(Interop.Go PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 90)
endif()
