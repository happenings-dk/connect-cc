# Ephemeral test-only key material stays in the build tree.
find_program(CONNECTCPP_OPENSSL openssl HINTS "${OPENSSL_ROOT_DIR}/bin" REQUIRED)
set(CONNECTCPP_TEST_CERT "${CMAKE_CURRENT_BINARY_DIR}/test-cert.pem")
set(CONNECTCPP_TEST_KEY "${CMAKE_CURRENT_BINARY_DIR}/test-key.pem")
if(NOT EXISTS "${CONNECTCPP_TEST_CERT}" OR NOT EXISTS "${CONNECTCPP_TEST_KEY}")
  execute_process(COMMAND "${CONNECTCPP_OPENSSL}" req -x509 -newkey rsa:2048
    -nodes -keyout "${CONNECTCPP_TEST_KEY}" -out "${CONNECTCPP_TEST_CERT}"
    -days 30 -subj /CN=localhost
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
    -addext "extendedKeyUsage=serverAuth,clientAuth"
    RESULT_VARIABLE certificate_result OUTPUT_QUIET ERROR_QUIET)
  if(NOT certificate_result EQUAL 0)
    message(FATAL_ERROR "Cannot create ephemeral TLS test certificate")
  endif()
endif()
target_compile_definitions(connectcpp_test PRIVATE
  CONNECTCPP_TEST_CERT="${CONNECTCPP_TEST_CERT}"
  CONNECTCPP_TEST_KEY="${CONNECTCPP_TEST_KEY}")
