# Standalone build of protobuf-c-grpc2. It is intentionally independent of the
# top-level protobuf-c build so that distributions can package it separately:
# it needs an installed libprotobuf-c and libnghttp2v2 (nghttp2 version 2).
#
# Three libraries are built, each with its own pkg-config file, so they can
# be split into separate distribution packages:
#   protobuf-c-grpc2          the core (no event loop or TLS dependencies)
#   protobuf-c-grpc2-uv       libuv I/O system        (needs libuv)
#   protobuf-c-grpc2-openssl  OpenSSL secure transport (needs OpenSSL)
cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(protobuf-c-grpc2 VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF_C REQUIRED IMPORTED_TARGET libprotobuf-c)
pkg_check_modules(NGHTTP2V2 REQUIRED IMPORTED_TARGET libnghttp2v2)

option(BUILD_TESTS "Build the tests (need protoc and protoc-gen-c)" ON)
option(BUILD_EXAMPLES "Build the libuv example server and client" ON)
option(WITH_UV "Build the libuv adapter" ON)
option(WITH_OPENSSL "Build the OpenSSL adapter" ON)

if(WITH_UV)
  pkg_check_modules(LIBUV IMPORTED_TARGET libuv)
  if(NOT LIBUV_FOUND)
    message(STATUS "libuv not found: not building protobuf-c-grpc2-uv")
  endif()
endif()
if(WITH_OPENSSL)
  pkg_check_modules(OPENSSL IMPORTED_TARGET openssl)
  if(NOT OPENSSL_FOUND)
    message(STATUS "OpenSSL not found: not building protobuf-c-grpc2-openssl")
  endif()
endif()

set(PUBLIC_HEADERS_DIR protobuf-c-grpc2)

# Helper: generate and install lib<name>.pc from lib<name>.pc.in.
function(grpc2_pc name)
  set(prefix ${CMAKE_INSTALL_PREFIX})
  set(exec_prefix \${prefix})
  set(libdir ${CMAKE_INSTALL_FULL_LIBDIR})
  set(includedir ${CMAKE_INSTALL_FULL_INCLUDEDIR})
  set(VERSION ${PROJECT_VERSION})
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/lib${name}.pc.in lib${name}.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endfunction()

# ---- core ----
add_library(protobuf-c-grpc2
  protobuf-c-grpc2/protobuf-c-grpc2.c
  protobuf-c-grpc2/protobuf-c-grpc2-server.c)
target_include_directories(protobuf-c-grpc2 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/protobuf-c-grpc2>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(protobuf-c-grpc2 PUBLIC
  PkgConfig::PROTOBUF_C PkgConfig::NGHTTP2V2)
target_compile_options(protobuf-c-grpc2 PRIVATE -Wall -Wextra)
set_target_properties(protobuf-c-grpc2 PROPERTIES
  VERSION ${PROJECT_VERSION} SOVERSION 0)
install(TARGETS protobuf-c-grpc2
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
  protobuf-c-grpc2/protobuf-c-grpc2.h
  protobuf-c-grpc2/protobuf-c-grpc2-io.h
  protobuf-c-grpc2/protobuf-c-grpc2-builder.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/protobuf-c-grpc2)
grpc2_pc(protobuf-c-grpc2)

# ---- adapters ----
if(LIBUV_FOUND)
  add_library(protobuf-c-grpc2-uv uv/protobuf-c-grpc2-uv.c)
  target_link_libraries(protobuf-c-grpc2-uv PUBLIC protobuf-c-grpc2 PkgConfig::LIBUV)
  target_compile_options(protobuf-c-grpc2-uv PRIVATE -Wall -Wextra)
  set_target_properties(protobuf-c-grpc2-uv PROPERTIES
    VERSION ${PROJECT_VERSION} SOVERSION 0)
  install(TARGETS protobuf-c-grpc2-uv
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES protobuf-c-grpc2/protobuf-c-grpc2-uv.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/protobuf-c-grpc2)
  grpc2_pc(protobuf-c-grpc2-uv)
endif()
if(OPENSSL_FOUND)
  add_library(protobuf-c-grpc2-openssl openssl/protobuf-c-grpc2-openssl.c)
  target_link_libraries(protobuf-c-grpc2-openssl PUBLIC protobuf-c-grpc2
    PkgConfig::OPENSSL)
  target_compile_options(protobuf-c-grpc2-openssl PRIVATE -Wall -Wextra)
  set_target_properties(protobuf-c-grpc2-openssl PROPERTIES
    VERSION ${PROJECT_VERSION} SOVERSION 0)
  install(TARGETS protobuf-c-grpc2-openssl
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES protobuf-c-grpc2/protobuf-c-grpc2-openssl.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/protobuf-c-grpc2)
  grpc2_pc(protobuf-c-grpc2-openssl)
endif()

# ---- tests and examples ----
if(BUILD_TESTS OR BUILD_EXAMPLES)
  find_program(PROTOC protoc REQUIRED)
  find_program(PROTOC_GEN_C protoc-gen-c REQUIRED)
  enable_testing()
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/t
                      ${CMAKE_CURRENT_BINARY_DIR}/examples)
endif()

function(grpc2_protoc dir proto)   # generates ${dir}/${proto}.pb-c.{c,h}
  add_custom_command(
    OUTPUT ${dir}/${proto}.pb-c.c ${dir}/${proto}.pb-c.h
    COMMAND ${PROTOC} --plugin=protoc-gen-c=${PROTOC_GEN_C}
            -I${CMAKE_CURRENT_SOURCE_DIR}/${dir}
            --c_out=${CMAKE_CURRENT_BINARY_DIR}/${dir}
            ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/${proto}.proto
    DEPENDS ${dir}/${proto}.proto)
endfunction()

if(BUILD_TESTS)
  grpc2_protoc(t loopback)
  add_executable(test-loopback t/loopback.c
    ${CMAKE_CURRENT_BINARY_DIR}/t/loopback.pb-c.c)
  target_include_directories(test-loopback PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/t ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(test-loopback protobuf-c-grpc2)
  add_test(NAME loopback COMMAND test-loopback)

  add_executable(test-protocol t/protocol.c
    ${CMAKE_CURRENT_BINARY_DIR}/t/loopback.pb-c.c)
  target_include_directories(test-protocol PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/t ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(test-protocol protobuf-c-grpc2)
  add_test(NAME protocol COMMAND test-protocol)
  set_tests_properties(protocol PROPERTIES TIMEOUT 60)

  if(LIBUV_FOUND AND OPENSSL_FOUND)
    grpc2_protoc(examples greeter)
    add_executable(test-builder t/builder.c
      ${CMAKE_CURRENT_BINARY_DIR}/examples/greeter.pb-c.c)
    target_include_directories(test-builder PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/examples)
    target_link_libraries(test-builder protobuf-c-grpc2-uv
      protobuf-c-grpc2-openssl)
    add_test(NAME builder COMMAND test-builder)
    set_tests_properties(builder PROPERTIES TIMEOUT 60)
  endif()
endif()

if(BUILD_EXAMPLES AND LIBUV_FOUND)
  grpc2_protoc(examples greeter)
  foreach(prog libuv-server libuv-client)
    add_executable(${prog} examples/${prog}.c
      ${CMAKE_CURRENT_BINARY_DIR}/examples/greeter.pb-c.c)
    target_include_directories(${prog} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/examples
      ${CMAKE_CURRENT_BINARY_DIR}/examples)
    target_link_libraries(${prog} protobuf-c-grpc2-uv)
    if(OPENSSL_FOUND)
      target_link_libraries(${prog} protobuf-c-grpc2-openssl)
      target_compile_definitions(${prog} PRIVATE HAVE_OPENSSL=1)
    endif()
  endforeach()
  add_test(NAME examples-smoke
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/examples/smoke-test.sh
            $<TARGET_FILE:libuv-server> $<TARGET_FILE:libuv-client>)
  set_tests_properties(examples-smoke PROPERTIES TIMEOUT 60)
elseif(BUILD_EXAMPLES)
  message(STATUS "libuv not found: not building the examples")
endif()
