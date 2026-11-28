# Standalone build of protobuf-c-grpc2. It is intentionally independent of the
# top-level protobuf-c build so that distributions can package it separately:
# it needs an installed libprotobuf-c and libnghttp2v2 (nghttp2 version 2).
#
# Three libraries are built, each with its own pkg-config file, so they can
# be split into separate distribution packages:
#   protobuf-c-grpc2          the core (no event loop or TLS dependencies)
#   protobuf-c-grpc2-uv       libuv I/O system        (needs libuv)
#   protobuf-c-grpc2-poll     poll(2) I/O system      (POSIX only)
#   protobuf-c-grpc2-dispatch ProtobufCDispatch I/O system (needs protobuf-c-rpc)
#   protobuf-c-grpc2-glib     GLib main-loop I/O system (needs GLib >= 2.36)
#   protobuf-c-grpc2-openssl  OpenSSL secure transport (needs OpenSSL)
#   protobuf-c-grpc2-gnutls   GnuTLS secure transport  (needs GnuTLS)
cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(protobuf-c-grpc2 VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF_C REQUIRED IMPORTED_TARGET libprotobuf-c)
pkg_check_modules(NGHTTP2V2 REQUIRED IMPORTED_TARGET libnghttp2v2)

option(BUILD_TESTS "Build the tests (need protoc and protoc-gen-c)" ON)
option(BUILD_EXAMPLES "Build the libuv example server and client" ON)
option(WITH_UV "Build the libuv adapter" ON)
option(WITH_POLL "Build the poll(2) adapter" ON)
option(WITH_DISPATCH "Build the ProtobufCDispatch (protobuf-c-rpc) adapter" ON)
option(WITH_GLIB "Build the GLib main-loop adapter" ON)
option(WITH_OPENSSL "Build the OpenSSL adapter" ON)
option(WITH_GNUTLS "Build the GnuTLS adapter" ON)

if(WITH_UV)
  pkg_check_modules(LIBUV IMPORTED_TARGET libuv)
  if(NOT LIBUV_FOUND)
    message(STATUS "libuv not found: not building protobuf-c-grpc2-uv")
  endif()
endif()
if(WITH_DISPATCH AND UNIX)
  pkg_check_modules(PROTOBUF_C_RPC IMPORTED_TARGET libprotobuf-c-rpc)
  if(PROTOBUF_C_RPC_FOUND)
    # the adapter needs the prepare functions of the dispatch
    include(CheckSymbolExists)
    set(CMAKE_REQUIRED_INCLUDES ${PROTOBUF_C_RPC_INCLUDE_DIRS}
                                ${PROTOBUF_C_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES PkgConfig::PROTOBUF_C_RPC)
    check_symbol_exists(protobuf_c_rpc_dispatch_add_prepare
      protobuf-c-rpc/protobuf-c-rpc-dispatch.h DISPATCH_HAS_PREPARE)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
  endif()
  if(PROTOBUF_C_RPC_FOUND AND DISPATCH_HAS_PREPARE)
    set(DISPATCH_FOUND TRUE)
  else()
    message(STATUS "protobuf-c-rpc with prepare functions not found: "
                   "not building protobuf-c-grpc2-dispatch")
  endif()
endif()
if(WITH_GLIB AND UNIX)
  pkg_check_modules(GLIB2 IMPORTED_TARGET glib-2.0>=2.36 gio-2.0)
  if(NOT GLIB2_FOUND)
    message(STATUS "GLib 2.36 with GIO not found: not building protobuf-c-grpc2-glib")
  endif()
endif()
if(WITH_OPENSSL)
  pkg_check_modules(OPENSSL IMPORTED_TARGET openssl)
  if(NOT OPENSSL_FOUND)
    message(STATUS "OpenSSL not found: not building protobuf-c-grpc2-openssl")
  endif()
endif()

if(WITH_GNUTLS)
  pkg_check_modules(GNUTLS IMPORTED_TARGET gnutls)
  if(NOT GNUTLS_FOUND)
    message(STATUS "GnuTLS not found: not building protobuf-c-grpc2-gnutls")
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
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
add_library(protobuf-c-grpc2
  protobuf-c-grpc2/protobuf-c-grpc2.c
  protobuf-c-grpc2/protobuf-c-grpc2-pump.c
  protobuf-c-grpc2/protobuf-c-grpc2-server.c
  protobuf-c-grpc2/protobuf-c-grpc2-client.c)
target_include_directories(protobuf-c-grpc2 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/protobuf-c-grpc2>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(protobuf-c-grpc2 PUBLIC
  PkgConfig::PROTOBUF_C PkgConfig::NGHTTP2V2 Threads::Threads)
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
if(WITH_POLL AND UNIX)
  set(POLL_FOUND TRUE)
  add_library(protobuf-c-grpc2-poll poll/protobuf-c-grpc2-poll.c)
  target_link_libraries(protobuf-c-grpc2-poll PUBLIC protobuf-c-grpc2)
  target_compile_options(protobuf-c-grpc2-poll PRIVATE -Wall -Wextra)
  set_target_properties(protobuf-c-grpc2-poll PROPERTIES
    VERSION ${PROJECT_VERSION} SOVERSION 0)
  install(TARGETS protobuf-c-grpc2-poll
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES protobuf-c-grpc2/protobuf-c-grpc2-poll.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/protobuf-c-grpc2)
  grpc2_pc(protobuf-c-grpc2-poll)
endif()
if(DISPATCH_FOUND)
  add_library(protobuf-c-grpc2-dispatch dispatch/protobuf-c-grpc2-dispatch.c)
  target_link_libraries(protobuf-c-grpc2-dispatch PUBLIC protobuf-c-grpc2
    PkgConfig::PROTOBUF_C_RPC)
  target_compile_options(protobuf-c-grpc2-dispatch PRIVATE -Wall -Wextra)
  set_target_properties(protobuf-c-grpc2-dispatch PROPERTIES
    VERSION ${PROJECT_VERSION} SOVERSION 0)
  install(TARGETS protobuf-c-grpc2-dispatch
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES protobuf-c-grpc2/protobuf-c-grpc2-dispatch.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/protobuf-c-grpc2)
  grpc2_pc(protobuf-c-grpc2-dispatch)
endif()
if(GLIB2_FOUND)
  add_library(protobuf-c-grpc2-glib glib/protobuf-c-grpc2-glib.c)
  target_link_libraries(protobuf-c-grpc2-glib PUBLIC protobuf-c-grpc2
    PkgConfig::GLIB2)
  target_compile_options(protobuf-c-grpc2-glib PRIVATE -Wall -Wextra)
  set_target_properties(protobuf-c-grpc2-glib PROPERTIES
    VERSION ${PROJECT_VERSION} SOVERSION 0)
  install(TARGETS protobuf-c-grpc2-glib
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES protobuf-c-grpc2/protobuf-c-grpc2-glib.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/protobuf-c-grpc2)
  grpc2_pc(protobuf-c-grpc2-glib)
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

if(GNUTLS_FOUND)
  add_library(protobuf-c-grpc2-gnutls gnutls/protobuf-c-grpc2-gnutls.c)
  target_link_libraries(protobuf-c-grpc2-gnutls PUBLIC protobuf-c-grpc2
    PkgConfig::GNUTLS)
  target_compile_options(protobuf-c-grpc2-gnutls PRIVATE -Wall -Wextra)
  set_target_properties(protobuf-c-grpc2-gnutls PROPERTIES
    VERSION ${PROJECT_VERSION} SOVERSION 0)
  install(TARGETS protobuf-c-grpc2-gnutls
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES protobuf-c-grpc2/protobuf-c-grpc2-gnutls.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/protobuf-c-grpc2)
  grpc2_pc(protobuf-c-grpc2-gnutls)
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

# The I/O backends the loop tests run on: the test sources are linked with
# t/loop-backend-<name>.c. "dispatch-embed" is the dispatch adapter driven by
# an application's own main-loop; it uses the same library as "dispatch".
set(BACKENDS "")
if(LIBUV_FOUND)
  list(APPEND BACKENDS uv)
endif()
if(POLL_FOUND)
  list(APPEND BACKENDS poll)
endif()
if(DISPATCH_FOUND)
  list(APPEND BACKENDS dispatch dispatch-embed)
endif()
if(GLIB2_FOUND)
  list(APPEND BACKENDS glib)
endif()

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

  if(POLL_FOUND)
    add_executable(test-poll t/poll.c)
    target_link_libraries(test-poll protobuf-c-grpc2-poll)
    add_test(NAME poll COMMAND test-poll)
    set_tests_properties(poll PROPERTIES TIMEOUT 60)
  endif()

  if(OPENSSL_FOUND AND GNUTLS_FOUND)
    add_executable(test-tls-interop t/tls-interop.c)
    target_include_directories(test-tls-interop PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/t)
    target_link_libraries(test-tls-interop
      protobuf-c-grpc2-openssl protobuf-c-grpc2-gnutls)
    add_test(NAME tls-interop COMMAND test-tls-interop)
    set_tests_properties(tls-interop PROPERTIES TIMEOUT 60)
  endif()

  if(GLIB2_FOUND)
    add_executable(test-glib t/glib.c)
    target_include_directories(test-glib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/t)
    target_link_libraries(test-glib protobuf-c-grpc2-glib)
    add_test(NAME glib COMMAND test-glib)
    set_tests_properties(glib PROPERTIES TIMEOUT 60)
  endif()

  if(DISPATCH_FOUND)
    add_executable(test-dispatch t/dispatch.c)
    target_link_libraries(test-dispatch protobuf-c-grpc2-dispatch)
    add_test(NAME dispatch COMMAND test-dispatch)
    set_tests_properties(dispatch PROPERTIES TIMEOUT 60)
  endif()

  # The endpoint and client tests run once on every I/O backend: the same
  # sources, linked with t/loop-backend-<name>.c.
  if(BACKENDS)
    grpc2_protoc(examples greeter)
  endif()
  foreach(backend ${BACKENDS})
    string(REGEX REPLACE "-embed$" "" lib ${backend})
    # The endpoint test runs on every TLS library as well, through the shim
    # in t/tls-backend.h.
    foreach(tls openssl gnutls)
      string(TOUPPER ${tls} TLS_UP)
      if(${TLS_UP}_FOUND)
        if(tls STREQUAL "openssl")
          set(ep test-endpoints-${backend})
        else()
          set(ep test-endpoints-${backend}-${tls})
        endif()
        add_executable(${ep} t/endpoints.c
          t/loop-backend-${backend}.c t/tls-backend-${tls}.c
          ${CMAKE_CURRENT_BINARY_DIR}/examples/greeter.pb-c.c)
        target_include_directories(${ep} PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR}/t
          ${CMAKE_CURRENT_BINARY_DIR}/examples)
        target_link_libraries(${ep}
          protobuf-c-grpc2-${lib} protobuf-c-grpc2-${tls})
        string(REGEX REPLACE "^test-" "" ep_name ${ep})
        add_test(NAME ${ep_name} COMMAND ${ep})
        set_tests_properties(${ep_name} PROPERTIES TIMEOUT 60)
      endif()
    endforeach()

    add_executable(test-bulk-${backend} t/bulk.c
      t/loop-backend-${backend}.c
      ${CMAKE_CURRENT_BINARY_DIR}/examples/greeter.pb-c.c)
    target_include_directories(test-bulk-${backend} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/t
      ${CMAKE_CURRENT_BINARY_DIR}/examples)
    target_link_libraries(test-bulk-${backend}
      protobuf-c-grpc2-${lib})
    add_test(NAME bulk-${backend} COMMAND test-bulk-${backend})
    set_tests_properties(bulk-${backend} PROPERTIES TIMEOUT 60)

    if(NOT backend MATCHES "^dispatch")	# the dispatch cannot be woken from another thread
      add_executable(test-threads-${backend} t/threads.c
        t/loop-backend-${backend}.c
        ${CMAKE_CURRENT_BINARY_DIR}/examples/greeter.pb-c.c)
      target_include_directories(test-threads-${backend} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/t
        ${CMAKE_CURRENT_BINARY_DIR}/examples)
      target_link_libraries(test-threads-${backend}
        protobuf-c-grpc2-${lib})
      add_test(NAME threads-${backend} COMMAND test-threads-${backend})
      set_tests_properties(threads-${backend} PROPERTIES TIMEOUT 60)

      add_executable(test-threads-server-${backend} t/threads-server.c
        t/loop-backend-${backend}.c
        ${CMAKE_CURRENT_BINARY_DIR}/examples/greeter.pb-c.c)
      target_include_directories(test-threads-server-${backend} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/t
        ${CMAKE_CURRENT_BINARY_DIR}/examples)
      target_link_libraries(test-threads-server-${backend}
        protobuf-c-grpc2-${lib})
      add_test(NAME threads-server-${backend}
        COMMAND test-threads-server-${backend})
      set_tests_properties(threads-server-${backend} PROPERTIES TIMEOUT 60)
    endif()

    add_executable(test-client-${backend} t/client.c
      t/loop-backend-${backend}.c
      ${CMAKE_CURRENT_BINARY_DIR}/examples/greeter.pb-c.c)
    target_include_directories(test-client-${backend} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/t
      ${CMAKE_CURRENT_BINARY_DIR}/examples)
    target_link_libraries(test-client-${backend}
      protobuf-c-grpc2-${lib})
    add_test(NAME client-${backend} COMMAND test-client-${backend})
    set_tests_properties(client-${backend} PROPERTIES TIMEOUT 60)
  endforeach()
endif()

# ---- stress: forks a server and N clients (see stress/stress.c) ----
if(BUILD_TESTS AND BACKENDS)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/stress)
  grpc2_protoc(stress stress)
  set(STRESS_BINARIES "")
  foreach(backend ${BACKENDS})
    string(REGEX REPLACE "-embed$" "" lib ${backend})
    add_executable(stress-${backend} stress/stress.c
      t/loop-backend-${backend}.c
      ${CMAKE_CURRENT_BINARY_DIR}/stress/stress.pb-c.c)
    target_include_directories(stress-${backend} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/t
      ${CMAKE_CURRENT_BINARY_DIR}/stress)
    target_link_libraries(stress-${backend} protobuf-c-grpc2-${lib})
    list(APPEND STRESS_BINARIES $<TARGET_FILE:stress-${backend}>)
  endforeach()
  add_test(NAME stress-smoke
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/stress/smoke.sh ${STRESS_BINARIES})
  set_tests_properties(stress-smoke PROPERTIES TIMEOUT 300)
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
  message(STATUS "libuv not found: not building the libuv examples")
endif()

# gRPC in a thread of its own, on the poll adapter (no other dependencies)
if(BUILD_EXAMPLES AND POLL_FOUND AND PROTOC AND PROTOC_GEN_C)
  grpc2_protoc(examples greeter)
  add_executable(background-rpc examples/background-rpc.c
    ${CMAKE_CURRENT_BINARY_DIR}/examples/greeter.pb-c.c)
  target_include_directories(background-rpc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/examples
    ${CMAKE_CURRENT_BINARY_DIR}/examples)
  target_link_libraries(background-rpc protobuf-c-grpc2-poll)
  if(OPENSSL_FOUND)
    target_link_libraries(background-rpc protobuf-c-grpc2-openssl)
    target_compile_definitions(background-rpc PRIVATE HAVE_OPENSSL=1)
  endif()
  add_test(NAME examples-background-rpc COMMAND background-rpc)
  set_tests_properties(examples-background-rpc PROPERTIES TIMEOUT 60)
endif()
