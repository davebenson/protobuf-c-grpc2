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

# The core is built against an *installed* libprotobuf-c and nghttp2-v2, so
# it can be released and packaged separately from protobuf-c itself.
PKG_CHECK_MODULES([PROTOBUF_C], [libprotobuf-c])
PKG_CHECK_MODULES([NGHTTP2V2], [libnghttp2v2])

# Optional adapter libraries; each can be packaged on its own.
AC_ARG_WITH([uv], AS_HELP_STRING([--without-uv], [do not build the libuv adapter]))
have_uv=no
AS_IF([test "x$with_uv" != xno],
      [PKG_CHECK_MODULES([LIBUV], [libuv], [have_uv=yes], [have_uv=no])])
AM_CONDITIONAL([WITH_UV], [test "$have_uv" = yes])

AC_ARG_WITH([openssl], AS_HELP_STRING([--without-openssl], [do not build the OpenSSL adapter]))
have_openssl=no
AS_IF([test "x$with_openssl" != xno],
      [PKG_CHECK_MODULES([OPENSSL], [openssl], [have_openssl=yes], [have_openssl=no])])
AM_CONDITIONAL([WITH_OPENSSL], [test "$have_openssl" = yes])

# Tests and examples are generated with protoc and protoc-gen-c.
AC_ARG_WITH([protoc], AS_HELP_STRING([--with-protoc=PATH], [protoc used to build tests]))
AC_ARG_WITH([protoc-gen-c], AS_HELP_STRING([--with-protoc-gen-c=PATH], [protoc-gen-c used to build tests]))
AS_IF([test "x$with_protoc" != x], [PROTOC="$with_protoc"], [AC_PATH_PROG([PROTOC], [protoc])])
AS_IF([test "x$with_protoc_gen_c" != x], [PROTOC_GEN_C="$with_protoc_gen_c"], [AC_PATH_PROG([PROTOC_GEN_C], [protoc-gen-c])])
AC_SUBST([PROTOC])
AC_SUBST([PROTOC_GEN_C])
AC_ARG_ENABLE([examples], AS_HELP_STRING([--disable-examples], [do not build the libuv examples]))
AM_CONDITIONAL([BUILD_TESTS], [test -n "$PROTOC" -a -n "$PROTOC_GEN_C"])
AM_CONDITIONAL([BUILD_BUILDER_TEST],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_uv" = yes -a "$have_openssl" = yes])
AM_CONDITIONAL([BUILD_EXAMPLES],
               [test "x$enable_examples" != xno -a "$have_uv" = yes -a -n "$PROTOC" -a -n "$PROTOC_GEN_C"])
AM_CONDITIONAL([EXAMPLES_OPENSSL], [test "$have_openssl" = yes])

AC_CONFIG_FILES([Makefile libprotobuf-c-grpc2.pc libprotobuf-c-grpc2-uv.pc libprotobuf-c-grpc2-openssl.pc])
AC_OUTPUT
