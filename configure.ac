AC_PREREQ([2.63])

AC_INIT([protobuf-c-grpc2],
        [0.1.0],
        [https://github.com/protobuf-c/protobuf-c/issues],
        [protobuf-c-grpc2],
        [https://github.com/protobuf-c/protobuf-c])
AC_CONFIG_SRCDIR([protobuf-c-grpc2/protobuf-c-grpc2.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign 1.11 -Wall -Wno-portability silent-rules subdir-objects])
AM_SILENT_RULES([yes])
AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
LT_INIT

PKG_PROG_PKG_CONFIG
PKG_INSTALLDIR

# grpc2 is built against an *installed* libprotobuf-c and nghttp2-v2, so it
# can be released and packaged separately from protobuf-c itself.
PKG_CHECK_MODULES([PROTOBUF_C], [libprotobuf-c])
PKG_CHECK_MODULES([NGHTTP2V2], [libnghttp2v2])

# The loopback test needs protoc and protoc-gen-c.
AC_ARG_WITH([protoc], AS_HELP_STRING([--with-protoc=PATH], [protoc used to build tests]))
AC_ARG_WITH([protoc-gen-c], AS_HELP_STRING([--with-protoc-gen-c=PATH], [protoc-gen-c used to build tests]))
AS_IF([test "x$with_protoc" != x], [PROTOC="$with_protoc"], [AC_PATH_PROG([PROTOC], [protoc])])
AS_IF([test "x$with_protoc_gen_c" != x], [PROTOC_GEN_C="$with_protoc_gen_c"], [AC_PATH_PROG([PROTOC_GEN_C], [protoc-gen-c])])
AM_CONDITIONAL([BUILD_TESTS], [test -n "$PROTOC" -a -n "$PROTOC_GEN_C"])
AC_SUBST([PROTOC])
AC_SUBST([PROTOC_GEN_C])

AC_CONFIG_FILES([Makefile libprotobuf-c-grpc2.pc])
AC_OUTPUT
