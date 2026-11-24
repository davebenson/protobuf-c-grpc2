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
	libprotobuf-c-grpc2-openssl.pc.in \
	protobuf-c-grpc2/protobuf-c-grpc2-internal.h \
	t/loopback.proto t/loopback.c t/builder.c t/client.c t/protocol.c \
	examples/greeter.proto examples/libuv-server.c \
	examples/libuv-client.c examples/smoke-test.sh

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
	protobuf-c-grpc2/protobuf-c-grpc2-io.h \
	protobuf-c-grpc2/protobuf-c-grpc2-builder.h
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

# examples/greeter.pb-c.[ch] is shared by the builder test and the examples.
if BUILD_BUILDER_TEST
check_PROGRAMS += t/test-builder
TESTS += t/test-builder
t_test_builder_SOURCES = t/builder.c
nodist_t_test_builder_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_builder_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) $(OPENSSL_CFLAGS) -I$(builddir)/examples
t_test_builder_LDADD = libprotobuf-c-grpc2-uv.la libprotobuf-c-grpc2-openssl.la \
	libprotobuf-c-grpc2.la $(LIBUV_LIBS) $(OPENSSL_LIBS)

check_PROGRAMS += t/test-client
TESTS += t/test-client
t_test_client_SOURCES = t/client.c
nodist_t_test_client_SOURCES = examples/greeter.pb-c.c examples/greeter.pb-c.h
t_test_client_CPPFLAGS = $(AM_CPPFLAGS) $(LIBUV_CFLAGS) -I$(builddir)/examples
t_test_client_LDADD = libprotobuf-c-grpc2-uv.la libprotobuf-c-grpc2.la $(LIBUV_LIBS)
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
if BUILD_BUILDER_TEST
BUILT_SOURCES += examples/greeter.pb-c.c examples/greeter.pb-c.h
else
if BUILD_EXAMPLES
BUILT_SOURCES += examples/greeter.pb-c.c examples/greeter.pb-c.h
endif
endif
