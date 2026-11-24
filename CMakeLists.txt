# Standalone build of protobuf-c-grpc2. It is intentionally independent of the
# top-level protobuf-c build so that distributions can package it separately:
# it needs an installed libprotobuf-c and libnghttp2v2 (nghttp2 version 2).
cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(protobuf-c-grpc2 VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF_C REQUIRED IMPORTED_TARGET libprotobuf-c)
pkg_check_modules(NGHTTP2V2 REQUIRED IMPORTED_TARGET libnghttp2v2)

option(BUILD_TESTS "Build the loopback test (needs protoc and protoc-gen-c)" ON)

add_library(protobuf-c-grpc2
  protobuf-c-grpc2/protobuf-c-grpc2.c
  protobuf-c-grpc2/protobuf-c-grpc2.h)
target_include_directories(protobuf-c-grpc2 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
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
install(FILES protobuf-c-grpc2/protobuf-c-grpc2.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/protobuf-c-grpc2)
set(prefix ${CMAKE_INSTALL_PREFIX})
set(exec_prefix \${prefix})
set(libdir ${CMAKE_INSTALL_FULL_LIBDIR})
set(includedir ${CMAKE_INSTALL_FULL_INCLUDEDIR})
set(VERSION ${PROJECT_VERSION})
configure_file(libprotobuf-c-grpc2.pc.in libprotobuf-c-grpc2.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libprotobuf-c-grpc2.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(BUILD_TESTS)
  enable_testing()
  find_program(PROTOC protoc REQUIRED)
  find_program(PROTOC_GEN_C protoc-gen-c REQUIRED)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/t)
  add_custom_command(
    OUTPUT t/loopback.pb-c.c t/loopback.pb-c.h
    COMMAND ${PROTOC} --plugin=protoc-gen-c=${PROTOC_GEN_C}
            -I${CMAKE_CURRENT_SOURCE_DIR}
            --c_out=${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/t/loopback.proto
    DEPENDS t/loopback.proto)
  add_executable(test-loopback t/loopback.c
    ${CMAKE_CURRENT_BINARY_DIR}/t/loopback.pb-c.c)
  target_include_directories(test-loopback PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(test-loopback protobuf-c-grpc2)
  add_test(NAME loopback COMMAND test-loopback)
endif()
