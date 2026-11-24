ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_builddir) $(PROTOBUF_C_CFLAGS) $(NGHTTP2V2_CFLAGS)
AM_CFLAGS = -Wall -Wextra

lib_LTLIBRARIES = libprotobuf-c-grpc2.la
libprotobuf_c_grpc2_la_SOURCES = protobuf-c-grpc2/protobuf-c-grpc2.c
libprotobuf_c_grpc2_la_LIBADD = $(PROTOBUF_C_LIBS) $(NGHTTP2V2_LIBS)
libprotobuf_c_grpc2_la_LDFLAGS = -version-info 0:0:0 -no-undefined

grpc2includedir = $(includedir)/protobuf-c-grpc2
grpc2include_HEADERS = protobuf-c-grpc2/protobuf-c-grpc2.h

pkgconfig_DATA = libprotobuf-c-grpc2.pc
EXTRA_DIST = CMakeLists.txt libprotobuf-c-grpc2.pc.in t/loopback.proto t/loopback.c

if BUILD_TESTS
check_PROGRAMS = t/test-loopback
TESTS = t/test-loopback
t_test_loopback_SOURCES = t/loopback.c t/loopback.pb-c.c
nodist_t_test_loopback_SOURCES = t/loopback.pb-c.h
t_test_loopback_LDADD = libprotobuf-c-grpc2.la

t/loopback.pb-c.c t/loopback.pb-c.h: $(top_srcdir)/t/loopback.proto
	$(AM_V_GEN)$(MKDIR_P) t && $(PROTOC) --plugin=protoc-gen-c=$(PROTOC_GEN_C) -I$(top_srcdir) --c_out=$(top_builddir) $(top_srcdir)/t/loopback.proto
BUILT_SOURCES = t/loopback.pb-c.c t/loopback.pb-c.h
CLEANFILES = $(BUILT_SOURCES)
endif
