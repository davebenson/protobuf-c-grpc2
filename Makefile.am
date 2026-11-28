ACLOCAL_AMFLAGS = -I m4

check_PROGRAMS =
TESTS =
BUILT_SOURCES =
CLEANFILES =
lib_LTLIBRARIES =
pkgconfig_DATA =
grpc2include_HEADERS =

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/protobuf-c-grpc2 -I$(top_builddir) \
	$(PROTOBUF_C_CFLAGS) $(NGHTTP2V2_CFLAGS)
AM_CFLAGS = -Wall -Wextra
grpc2includedir = $(includedir)/protobuf-c-grpc2

EXTRA_DIST = CMakeLists.txt \
	libprotobuf-c-grpc2.pc.in libprotobuf-c-grpc2-uv.pc.in \
	libprotobuf-c-grpc2-openssl.pc.in libprotobuf-c-grpc2-poll.pc.in \
	libprotobuf-c-grpc2-dispatch.pc.in libprotobuf-c-grpc2-gnutls.pc.in \
	libprotobuf-c-grpc2-glib.pc.in \
	protobuf-c-grpc2/protobuf-c-grpc2-internal.h \
	t/loopback.proto t/loopback.c t/endpoints.c t/client.c t/protocol.c \
	t/bulk.c t/poll.c t/dispatch.c t/loop-backend.h t/loop-backend-uv.c t/loop-backend-poll.c \
	t/loop-backend-dispatch.c t/loop-backend-dispatch-embed.c \
	t/threads.c t/threads-server.c t/glib.c t/glib-iterate.h t/loop-backend-glib.c \
	t/free-port.h t/tls-backend.h t/tls-backend-openssl.c t/tls-backend-gnutls.c t/tls-test-certs.h t/tls-interop.c \
	examples/greeter.proto examples/libuv-server.c \
	examples/libuv-client.c examples/smoke-test.sh \
	stress/stress.proto stress/stress.c stress/smoke.sh

# ---- core: no event loop or TLS dependencies ----
lib_LTLIBRARIES += libprotobuf-c-grpc2.la
libprotobuf_c_grpc2_la_SOURCES = \
	protobuf-c-grpc2/protobuf-c-grpc2.c \
	protobuf-c-grpc2/protobuf-c-grpc2-pump.c \
	protobuf-c-grpc2/protobuf-c-grpc2-server.c \
	protobuf-c-grpc2/protobuf-c-grpc2-client.c
libprotobuf_c_grpc2_la_LIBADD = $(PROTOBUF_C_LIBS) $(NGHTTP2V2_LIBS)
libprotobuf_c_grpc2_la_LDFLAGS = -version-info 0:0:0 -no-undefined
grpc2include_HEADERS += \
	protobuf-c-grpc2/protobuf-c-grpc2.h \
	protobuf-c-grpc2/protobuf-c-grpc2-io.h
pkgconfig_DATA += libprotobuf-c-grpc2.pc

# ---- libuv adapter ----
if WITH_UV
lib_LTLIBRARIES += libprotobuf-c-grpc2-uv.la
libprotobuf_c_grpc2_uv_la_SOURCES = uv/protobuf-c-grpc2-uv.c
libprotobuf_c_grpc2_uv_la_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS)
libprotobuf_c_grpc2_uv_la_LIBADD = libprotobuf-c-grpc2.la $(LIBUV_LIBS)
libprotobuf_c_grpc2_uv_la_LDFLAGS = -version-info 0:0:0 -no-undefined
grpc2include_HEADERS += protobuf-c-grpc2/protobuf-c-grpc2-uv.h
pkgconfig_DATA += libprotobuf-c-grpc2-uv.pc
endif

# ---- poll(2) adapter ----
if WITH_POLL
lib_LTLIBRARIES += libprotobuf-c-grpc2-poll.la
libprotobuf_c_grpc2_poll_la_SOURCES = poll/protobuf-c-grpc2-poll.c
libprotobuf_c_grpc2_poll_la_LIBADD = libprotobuf-c-grpc2.la
libprotobuf_c_grpc2_poll_la_LDFLAGS = -version-info 0:0:0 -no-undefined
grpc2include_HEADERS += protobuf-c-grpc2/protobuf-c-grpc2-poll.h
pkgconfig_DATA += libprotobuf-c-grpc2-poll.pc
endif

# ---- ProtobufCDispatch (protobuf-c-rpc) adapter ----
if WITH_DISPATCH
lib_LTLIBRARIES += libprotobuf-c-grpc2-dispatch.la
libprotobuf_c_grpc2_dispatch_la_SOURCES = dispatch/protobuf-c-grpc2-dispatch.c
libprotobuf_c_grpc2_dispatch_la_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS)
libprotobuf_c_grpc2_dispatch_la_LIBADD = libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS)
libprotobuf_c_grpc2_dispatch_la_LDFLAGS = -version-info 0:0:0 -no-undefined
grpc2include_HEADERS += protobuf-c-grpc2/protobuf-c-grpc2-dispatch.h
pkgconfig_DATA += libprotobuf-c-grpc2-dispatch.pc
endif
if WITH_GLIB
lib_LTLIBRARIES += libprotobuf-c-grpc2-glib.la
libprotobuf_c_grpc2_glib_la_SOURCES = glib/protobuf-c-grpc2-glib.c
libprotobuf_c_grpc2_glib_la_CPPFLAGS = $(AM_CPPFLAGS) $(GLIB_CFLAGS)
libprotobuf_c_grpc2_glib_la_LIBADD = libprotobuf-c-grpc2.la $(GLIB_LIBS)
libprotobuf_c_grpc2_glib_la_LDFLAGS = -version-info 0:0:0 -no-undefined
grpc2include_HEADERS += protobuf-c-grpc2/protobuf-c-grpc2-glib.h
pkgconfig_DATA += libprotobuf-c-grpc2-glib.pc
endif

# ---- OpenSSL adapter ----
if WITH_OPENSSL
lib_LTLIBRARIES += libprotobuf-c-grpc2-openssl.la
libprotobuf_c_grpc2_openssl_la_SOURCES = openssl/protobuf-c-grpc2-openssl.c
libprotobuf_c_grpc2_openssl_la_CPPFLAGS = $(AM_CPPFLAGS) $(OPENSSL_CFLAGS)
libprotobuf_c_grpc2_openssl_la_LIBADD = libprotobuf-c-grpc2.la $(OPENSSL_LIBS)
libprotobuf_c_grpc2_openssl_la_LDFLAGS = -version-info 0:0:0 -no-undefined
grpc2include_HEADERS += protobuf-c-grpc2/protobuf-c-grpc2-openssl.h
pkgconfig_DATA += libprotobuf-c-grpc2-openssl.pc
endif

# ---- GnuTLS adapter ----
if WITH_GNUTLS
lib_LTLIBRARIES += libprotobuf-c-grpc2-gnutls.la
libprotobuf_c_grpc2_gnutls_la_SOURCES = gnutls/protobuf-c-grpc2-gnutls.c
libprotobuf_c_grpc2_gnutls_la_CPPFLAGS = $(AM_CPPFLAGS) $(GNUTLS_CFLAGS)
libprotobuf_c_grpc2_gnutls_la_LIBADD = libprotobuf-c-grpc2.la $(GNUTLS_LIBS)
libprotobuf_c_grpc2_gnutls_la_LDFLAGS = -version-info 0:0:0 -no-undefined
grpc2include_HEADERS += protobuf-c-grpc2/protobuf-c-grpc2-gnutls.h
pkgconfig_DATA += libprotobuf-c-grpc2-gnutls.pc
endif

# ---- tests ----
if BUILD_TESTS
check_PROGRAMS += t/test-loopback
TESTS += t/test-loopback
t_test_loopback_SOURCES = t/loopback.c t/loopback.pb-c.c
nodist_t_test_loopback_SOURCES = t/loopback.pb-c.h
t_test_loopback_LDADD = libprotobuf-c-grpc2.la

check_PROGRAMS += t/test-protocol
TESTS += t/test-protocol
t_test_protocol_SOURCES = t/protocol.c t/loopback.pb-c.c
nodist_t_test_protocol_SOURCES = t/loopback.pb-c.h
t_test_protocol_LDADD = libprotobuf-c-grpc2.la

t/loopback.pb-c.c t/loopback.pb-c.h: $(top_srcdir)/t/loopback.proto
	$(AM_V_GEN)$(MKDIR_P) t && $(PROTOC) --plugin=protoc-gen-c=$(PROTOC_GEN_C) -I$(top_srcdir)/t --c_out=$(top_builddir)/t $(top_srcdir)/t/loopback.proto
BUILT_SOURCES += t/loopback.pb-c.c t/loopback.pb-c.h
CLEANFILES += t/loopback.pb-c.c t/loopback.pb-c.h
endif

# examples/greeter.pb-c.[ch] is shared by the endpoint tests and the examples.
# The endpoint, bulk and client tests run once per I/O backend: the same
# sources, linked with t/loop-backend-<name>.c.
if BUILD_UV_TESTS
check_PROGRAMS += t/test-client-uv t/test-bulk-uv
TESTS += t/test-client-uv t/test-bulk-uv
t_test_client_uv_SOURCES = t/client.c t/loop-backend-uv.c
nodist_t_test_client_uv_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_client_uv_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_client_uv_LDADD = libprotobuf-c-grpc2-uv.la libprotobuf-c-grpc2.la $(LIBUV_LIBS)
t_test_bulk_uv_SOURCES = t/bulk.c t/loop-backend-uv.c
nodist_t_test_bulk_uv_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_bulk_uv_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_bulk_uv_LDADD = libprotobuf-c-grpc2-uv.la libprotobuf-c-grpc2.la $(LIBUV_LIBS)
endif
if BUILD_ENDPOINTS_TEST
check_PROGRAMS += t/test-endpoints-uv
TESTS += t/test-endpoints-uv
t_test_endpoints_uv_SOURCES = t/endpoints.c t/loop-backend-uv.c t/tls-backend-openssl.c
nodist_t_test_endpoints_uv_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_endpoints_uv_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) $(OPENSSL_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_endpoints_uv_LDADD = libprotobuf-c-grpc2-uv.la libprotobuf-c-grpc2-openssl.la \
	libprotobuf-c-grpc2.la $(LIBUV_LIBS) $(OPENSSL_LIBS)
endif
if BUILD_POLL_TESTS
check_PROGRAMS += t/test-poll t/test-client-poll t/test-bulk-poll
TESTS += t/test-poll t/test-client-poll t/test-bulk-poll
t_test_poll_SOURCES = t/poll.c
t_test_poll_LDADD = libprotobuf-c-grpc2-poll.la libprotobuf-c-grpc2.la
t_test_client_poll_SOURCES = t/client.c t/loop-backend-poll.c
nodist_t_test_client_poll_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_client_poll_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_client_poll_LDADD = libprotobuf-c-grpc2-poll.la libprotobuf-c-grpc2.la
t_test_bulk_poll_SOURCES = t/bulk.c t/loop-backend-poll.c
nodist_t_test_bulk_poll_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_bulk_poll_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_bulk_poll_LDADD = libprotobuf-c-grpc2-poll.la libprotobuf-c-grpc2.la
endif
if BUILD_POLL_ENDPOINTS_TEST
check_PROGRAMS += t/test-endpoints-poll
TESTS += t/test-endpoints-poll
t_test_endpoints_poll_SOURCES = t/endpoints.c t/loop-backend-poll.c t/tls-backend-openssl.c
nodist_t_test_endpoints_poll_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_endpoints_poll_CPPFLAGS = $(AM_CPPFLAGS) $(OPENSSL_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_endpoints_poll_LDADD = libprotobuf-c-grpc2-poll.la libprotobuf-c-grpc2-openssl.la \
	libprotobuf-c-grpc2.la $(OPENSSL_LIBS)
endif

if BUILD_DISPATCH_TESTS
check_PROGRAMS += t/test-dispatch t/test-client-dispatch t/test-bulk-dispatch \
	t/test-client-dispatch-embed t/test-bulk-dispatch-embed
TESTS += t/test-dispatch t/test-client-dispatch t/test-bulk-dispatch \
	t/test-client-dispatch-embed t/test-bulk-dispatch-embed
t_test_dispatch_SOURCES = t/dispatch.c
t_test_dispatch_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS)
t_test_dispatch_LDADD = libprotobuf-c-grpc2-dispatch.la libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS)
t_test_client_dispatch_SOURCES = t/client.c t/loop-backend-dispatch.c
nodist_t_test_client_dispatch_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_client_dispatch_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_client_dispatch_LDADD = libprotobuf-c-grpc2-dispatch.la libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS)
t_test_bulk_dispatch_SOURCES = t/bulk.c t/loop-backend-dispatch.c
nodist_t_test_bulk_dispatch_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_bulk_dispatch_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_bulk_dispatch_LDADD = libprotobuf-c-grpc2-dispatch.la libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS)
t_test_client_dispatch_embed_SOURCES = t/client.c t/loop-backend-dispatch-embed.c
nodist_t_test_client_dispatch_embed_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_client_dispatch_embed_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_client_dispatch_embed_LDADD = libprotobuf-c-grpc2-dispatch.la libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS)
t_test_bulk_dispatch_embed_SOURCES = t/bulk.c t/loop-backend-dispatch-embed.c
nodist_t_test_bulk_dispatch_embed_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_bulk_dispatch_embed_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_bulk_dispatch_embed_LDADD = libprotobuf-c-grpc2-dispatch.la libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS)
endif
if BUILD_DISPATCH_ENDPOINTS_TEST
check_PROGRAMS += t/test-endpoints-dispatch t/test-endpoints-dispatch-embed
TESTS += t/test-endpoints-dispatch t/test-endpoints-dispatch-embed
t_test_endpoints_dispatch_SOURCES = t/endpoints.c t/loop-backend-dispatch.c t/tls-backend-openssl.c
nodist_t_test_endpoints_dispatch_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_endpoints_dispatch_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS) $(OPENSSL_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_endpoints_dispatch_LDADD = libprotobuf-c-grpc2-dispatch.la libprotobuf-c-grpc2-openssl.la \
	libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS) $(OPENSSL_LIBS)
t_test_endpoints_dispatch_embed_SOURCES = t/endpoints.c t/loop-backend-dispatch-embed.c t/tls-backend-openssl.c
nodist_t_test_endpoints_dispatch_embed_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_endpoints_dispatch_embed_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS) $(OPENSSL_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_endpoints_dispatch_embed_LDADD = libprotobuf-c-grpc2-dispatch.la libprotobuf-c-grpc2-openssl.la \
	libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS) $(OPENSSL_LIBS)
endif

if BUILD_GLIB_TESTS
check_PROGRAMS += t/test-glib t/test-client-glib t/test-bulk-glib
TESTS += t/test-glib t/test-client-glib t/test-bulk-glib
t_test_glib_SOURCES = t/glib.c
t_test_glib_CPPFLAGS = $(AM_CPPFLAGS) $(GLIB_CFLAGS) -I$(srcdir)/t
t_test_glib_LDADD = libprotobuf-c-grpc2-glib.la libprotobuf-c-grpc2.la $(GLIB_LIBS)
t_test_client_glib_SOURCES = t/client.c t/loop-backend-glib.c
nodist_t_test_client_glib_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_client_glib_CPPFLAGS = $(AM_CPPFLAGS) $(GLIB_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_client_glib_LDADD = libprotobuf-c-grpc2-glib.la libprotobuf-c-grpc2.la $(GLIB_LIBS)
t_test_bulk_glib_SOURCES = t/bulk.c t/loop-backend-glib.c
nodist_t_test_bulk_glib_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_bulk_glib_CPPFLAGS = $(AM_CPPFLAGS) $(GLIB_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_bulk_glib_LDADD = libprotobuf-c-grpc2-glib.la libprotobuf-c-grpc2.la $(GLIB_LIBS)
endif
# thread safe clients (the dispatch adapter cannot be woken from another thread)
if BUILD_UV_TESTS
check_PROGRAMS += t/test-threads-uv t/test-threads-server-uv
TESTS += t/test-threads-uv t/test-threads-server-uv
t_test_threads_uv_SOURCES = t/threads.c t/loop-backend-uv.c
nodist_t_test_threads_uv_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_threads_uv_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_threads_uv_LDADD = libprotobuf-c-grpc2-uv.la libprotobuf-c-grpc2.la $(LIBUV_LIBS)
t_test_threads_server_uv_SOURCES = t/threads-server.c t/loop-backend-uv.c
nodist_t_test_threads_server_uv_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_threads_server_uv_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_threads_server_uv_LDADD = libprotobuf-c-grpc2-uv.la libprotobuf-c-grpc2.la $(LIBUV_LIBS)
endif
if BUILD_POLL_TESTS
check_PROGRAMS += t/test-threads-poll t/test-threads-server-poll
TESTS += t/test-threads-poll t/test-threads-server-poll
t_test_threads_poll_SOURCES = t/threads.c t/loop-backend-poll.c
nodist_t_test_threads_poll_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_threads_poll_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_threads_poll_LDADD = libprotobuf-c-grpc2-poll.la libprotobuf-c-grpc2.la
t_test_threads_server_poll_SOURCES = t/threads-server.c t/loop-backend-poll.c
nodist_t_test_threads_server_poll_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_threads_server_poll_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_threads_server_poll_LDADD = libprotobuf-c-grpc2-poll.la libprotobuf-c-grpc2.la
endif
if BUILD_GLIB_TESTS
check_PROGRAMS += t/test-threads-glib t/test-threads-server-glib
TESTS += t/test-threads-glib t/test-threads-server-glib
t_test_threads_glib_SOURCES = t/threads.c t/loop-backend-glib.c
nodist_t_test_threads_glib_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_threads_glib_CPPFLAGS = $(AM_CPPFLAGS) $(GLIB_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_threads_glib_LDADD = libprotobuf-c-grpc2-glib.la libprotobuf-c-grpc2.la $(GLIB_LIBS)
t_test_threads_server_glib_SOURCES = t/threads-server.c t/loop-backend-glib.c
nodist_t_test_threads_server_glib_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_threads_server_glib_CPPFLAGS = $(AM_CPPFLAGS) $(GLIB_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_threads_server_glib_LDADD = libprotobuf-c-grpc2-glib.la libprotobuf-c-grpc2.la $(GLIB_LIBS)
endif
if BUILD_GLIB_ENDPOINTS_TEST
check_PROGRAMS += t/test-endpoints-glib
TESTS += t/test-endpoints-glib
t_test_endpoints_glib_SOURCES = t/endpoints.c t/loop-backend-glib.c t/tls-backend-openssl.c
nodist_t_test_endpoints_glib_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_endpoints_glib_CPPFLAGS = $(AM_CPPFLAGS) $(GLIB_CFLAGS) $(OPENSSL_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_endpoints_glib_LDADD = libprotobuf-c-grpc2-glib.la libprotobuf-c-grpc2-openssl.la \
	libprotobuf-c-grpc2.la $(GLIB_LIBS) $(OPENSSL_LIBS)
endif
if BUILD_GLIB_GNUTLS_ENDPOINTS_TEST
check_PROGRAMS += t/test-endpoints-glib-gnutls
TESTS += t/test-endpoints-glib-gnutls
t_test_endpoints_glib_gnutls_SOURCES = t/endpoints.c t/loop-backend-glib.c t/tls-backend-gnutls.c
nodist_t_test_endpoints_glib_gnutls_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_endpoints_glib_gnutls_CPPFLAGS = $(AM_CPPFLAGS) $(GLIB_CFLAGS) $(GNUTLS_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_endpoints_glib_gnutls_LDADD = libprotobuf-c-grpc2-glib.la \
	libprotobuf-c-grpc2-gnutls.la libprotobuf-c-grpc2.la $(GLIB_LIBS) $(GNUTLS_LIBS)
endif

# The endpoint test again on the GnuTLS adapter, and the two TLS adapters
# against each other.
if BUILD_GNUTLS_ENDPOINTS_TEST
check_PROGRAMS += t/test-endpoints-uv-gnutls
TESTS += t/test-endpoints-uv-gnutls
t_test_endpoints_uv_gnutls_SOURCES = t/endpoints.c t/loop-backend-uv.c t/tls-backend-gnutls.c
nodist_t_test_endpoints_uv_gnutls_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_endpoints_uv_gnutls_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) $(GNUTLS_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_endpoints_uv_gnutls_LDADD = libprotobuf-c-grpc2-uv.la \
	libprotobuf-c-grpc2-gnutls.la libprotobuf-c-grpc2.la $(LIBUV_LIBS) $(GNUTLS_LIBS)
endif
if BUILD_POLL_GNUTLS_ENDPOINTS_TEST
check_PROGRAMS += t/test-endpoints-poll-gnutls
TESTS += t/test-endpoints-poll-gnutls
t_test_endpoints_poll_gnutls_SOURCES = t/endpoints.c t/loop-backend-poll.c t/tls-backend-gnutls.c
nodist_t_test_endpoints_poll_gnutls_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_endpoints_poll_gnutls_CPPFLAGS = $(AM_CPPFLAGS) $(GNUTLS_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_endpoints_poll_gnutls_LDADD = libprotobuf-c-grpc2-poll.la \
	libprotobuf-c-grpc2-gnutls.la libprotobuf-c-grpc2.la $(GNUTLS_LIBS)
endif
if BUILD_DISPATCH_GNUTLS_ENDPOINTS_TEST
check_PROGRAMS += t/test-endpoints-dispatch-gnutls
TESTS += t/test-endpoints-dispatch-gnutls
t_test_endpoints_dispatch_gnutls_SOURCES = t/endpoints.c t/loop-backend-dispatch.c t/tls-backend-gnutls.c
nodist_t_test_endpoints_dispatch_gnutls_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_endpoints_dispatch_gnutls_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS) $(GNUTLS_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_endpoints_dispatch_gnutls_LDADD = libprotobuf-c-grpc2-dispatch.la \
	libprotobuf-c-grpc2-gnutls.la libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS) $(GNUTLS_LIBS)
endif
if BUILD_DISPATCH_GNUTLS_ENDPOINTS_TEST
check_PROGRAMS += t/test-endpoints-dispatch-embed-gnutls
TESTS += t/test-endpoints-dispatch-embed-gnutls
t_test_endpoints_dispatch_embed_gnutls_SOURCES = t/endpoints.c t/loop-backend-dispatch-embed.c t/tls-backend-gnutls.c
nodist_t_test_endpoints_dispatch_embed_gnutls_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_endpoints_dispatch_embed_gnutls_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS) $(GNUTLS_CFLAGS) -I$(srcdir)/t -I$(builddir)/examples
t_test_endpoints_dispatch_embed_gnutls_LDADD = libprotobuf-c-grpc2-dispatch.la \
	libprotobuf-c-grpc2-gnutls.la libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS) $(GNUTLS_LIBS)
endif
if BUILD_TLS_INTEROP_TEST
check_PROGRAMS += t/test-tls-interop
TESTS += t/test-tls-interop
t_test_tls_interop_SOURCES = t/tls-interop.c
t_test_tls_interop_CPPFLAGS = $(AM_CPPFLAGS) $(OPENSSL_CFLAGS) $(GNUTLS_CFLAGS) -I$(srcdir)/t
t_test_tls_interop_LDADD = libprotobuf-c-grpc2-openssl.la libprotobuf-c-grpc2-gnutls.la \
	libprotobuf-c-grpc2.la $(OPENSSL_LIBS) $(GNUTLS_LIBS)
endif

# ---- stress: forks a server and N clients (see stress/stress.c) ----
stress/stress.pb-c.c stress/stress.pb-c.h: $(top_srcdir)/stress/stress.proto
	$(AM_V_GEN)$(MKDIR_P) stress && $(PROTOC) --plugin=protoc-gen-c=$(PROTOC_GEN_C) -I$(top_srcdir)/stress --c_out=$(top_builddir)/stress $(top_srcdir)/stress/stress.proto
CLEANFILES += stress/stress.pb-c.c stress/stress.pb-c.h
if BUILD_UV_TESTS
BUILT_SOURCES += stress/stress.pb-c.c stress/stress.pb-c.h
check_PROGRAMS += stress/stress-uv
TESTS += stress/smoke.sh
stress_stress_uv_SOURCES = stress/stress.c t/loop-backend-uv.c
nodist_stress_stress_uv_SOURCES = stress/stress.pb-c.c stress/stress.pb-c.h
stress_stress_uv_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) -I$(srcdir)/t -I$(builddir)/stress
stress_stress_uv_LDADD = libprotobuf-c-grpc2-uv.la libprotobuf-c-grpc2.la $(LIBUV_LIBS)
endif
if BUILD_POLL_TESTS
BUILT_SOURCES += stress/stress.pb-c.c stress/stress.pb-c.h
check_PROGRAMS += stress/stress-poll
if !BUILD_UV_TESTS
TESTS += stress/smoke.sh
endif
stress_stress_poll_SOURCES = stress/stress.c t/loop-backend-poll.c
nodist_stress_stress_poll_SOURCES = stress/stress.pb-c.c stress/stress.pb-c.h
stress_stress_poll_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir)/t -I$(builddir)/stress
stress_stress_poll_LDADD = libprotobuf-c-grpc2-poll.la libprotobuf-c-grpc2.la
endif

if BUILD_DISPATCH_TESTS
BUILT_SOURCES += stress/stress.pb-c.c stress/stress.pb-c.h
check_PROGRAMS += stress/stress-dispatch stress/stress-dispatch-embed
if !BUILD_UV_TESTS
if !BUILD_POLL_TESTS
TESTS += stress/smoke.sh
endif
endif
stress_stress_dispatch_SOURCES = stress/stress.c t/loop-backend-dispatch.c
nodist_stress_stress_dispatch_SOURCES = stress/stress.pb-c.c stress/stress.pb-c.h
stress_stress_dispatch_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS) -I$(srcdir)/t -I$(builddir)/stress
stress_stress_dispatch_LDADD = libprotobuf-c-grpc2-dispatch.la libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS)
stress_stress_dispatch_embed_SOURCES = stress/stress.c t/loop-backend-dispatch-embed.c
nodist_stress_stress_dispatch_embed_SOURCES = stress/stress.pb-c.c stress/stress.pb-c.h
stress_stress_dispatch_embed_CPPFLAGS = $(AM_CPPFLAGS) $(PROTOBUF_C_RPC_CFLAGS) -I$(srcdir)/t -I$(builddir)/stress
stress_stress_dispatch_embed_LDADD = libprotobuf-c-grpc2-dispatch.la libprotobuf-c-grpc2.la $(PROTOBUF_C_RPC_LIBS) $(PROTOBUF_C_LIBS)
endif

if BUILD_GLIB_TESTS
BUILT_SOURCES += stress/stress.pb-c.c stress/stress.pb-c.h
check_PROGRAMS += stress/stress-glib
if !BUILD_UV_TESTS
if !BUILD_POLL_TESTS
if !BUILD_DISPATCH_TESTS
TESTS += stress/smoke.sh
endif
endif
endif
stress_stress_glib_SOURCES = stress/stress.c t/loop-backend-glib.c
nodist_stress_stress_glib_SOURCES = stress/stress.pb-c.c stress/stress.pb-c.h
stress_stress_glib_CPPFLAGS = $(AM_CPPFLAGS) $(GLIB_CFLAGS) -I$(srcdir)/t -I$(builddir)/stress
stress_stress_glib_LDADD = libprotobuf-c-grpc2-glib.la libprotobuf-c-grpc2.la $(GLIB_LIBS)
endif

# ---- examples ----
if BUILD_EXAMPLES
check_PROGRAMS += examples/libuv-server examples/libuv-client
TESTS += examples/smoke-test.sh

examples_libuv_server_SOURCES = examples/libuv-server.c
nodist_examples_libuv_server_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
examples_libuv_server_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) $(OPENSSL_CFLAGS) -I$(srcdir)/examples -I$(builddir)/examples
examples_libuv_server_LDADD = libprotobuf-c-grpc2-uv.la libprotobuf-c-grpc2.la $(LIBUV_LIBS)
if EXAMPLES_OPENSSL
examples_libuv_server_CPPFLAGS += -DHAVE_OPENSSL=1
examples_libuv_server_LDADD += libprotobuf-c-grpc2-openssl.la $(OPENSSL_LIBS)
endif

examples_libuv_client_SOURCES = examples/libuv-client.c
nodist_examples_libuv_client_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
examples_libuv_client_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) $(OPENSSL_CFLAGS) -I$(srcdir)/examples -I$(builddir)/examples
examples_libuv_client_LDADD = libprotobuf-c-grpc2-uv.la libprotobuf-c-grpc2.la $(LIBUV_LIBS)
if EXAMPLES_OPENSSL
examples_libuv_client_CPPFLAGS += -DHAVE_OPENSSL=1
examples_libuv_client_LDADD += libprotobuf-c-grpc2-openssl.la $(OPENSSL_LIBS)
endif
endif

examples/greeter.pb-c.c examples/greeter.pb-c.h: $(top_srcdir)/examples/greeter.proto
	$(AM_V_GEN)$(MKDIR_P) examples && $(PROTOC) --plugin=protoc-gen-c=$(PROTOC_GEN_C) -I$(top_srcdir)/examples --c_out=$(top_builddir)/examples $(top_srcdir)/examples/greeter.proto
CLEANFILES += examples/greeter.pb-c.c examples/greeter.pb-c.h
if BUILD_UV_TESTS
BUILT_SOURCES += examples/greeter.pb-c.c examples/greeter.pb-c.h
else
if BUILD_POLL_TESTS
BUILT_SOURCES += examples/greeter.pb-c.c examples/greeter.pb-c.h
else
if BUILD_DISPATCH_TESTS
BUILT_SOURCES += examples/greeter.pb-c.c examples/greeter.pb-c.h
else
if BUILD_GLIB_TESTS
BUILT_SOURCES += examples/greeter.pb-c.c examples/greeter.pb-c.h
else
if BUILD_EXAMPLES
BUILT_SOURCES += examples/greeter.pb-c.c examples/greeter.pb-c.h
endif
endif
endif
endif
endif
