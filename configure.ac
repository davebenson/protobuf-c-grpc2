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

# The poll(2) adapter needs nothing but POSIX sockets.
AC_ARG_WITH([poll], AS_HELP_STRING([--without-poll], [do not build the poll(2) adapter]))
AM_CONDITIONAL([WITH_POLL], [test "x$with_poll" != xno])

# The ProtobufCDispatch adapter needs protobuf-c-rpc with prepare functions.
AC_ARG_WITH([dispatch], AS_HELP_STRING([--without-dispatch], [do not build the ProtobufCDispatch adapter]))
have_dispatch=no
AS_IF([test "x$with_dispatch" != xno],
      [PKG_CHECK_MODULES([PROTOBUF_C_RPC], [libprotobuf-c-rpc], [have_dispatch=yes], [have_dispatch=no])
       AS_IF([test "$have_dispatch" = yes],
             [save_CPPFLAGS=$CPPFLAGS
              CPPFLAGS="$CPPFLAGS $PROTOBUF_C_RPC_CFLAGS $PROTOBUF_C_CFLAGS"
              AC_CHECK_DECL([protobuf_c_rpc_dispatch_add_prepare],
                            [], [have_dispatch=no],
                            [[#include <protobuf-c-rpc/protobuf-c-rpc-dispatch.h>]])
              CPPFLAGS=$save_CPPFLAGS
              AS_IF([test "$have_dispatch" = no],
                    [AC_MSG_NOTICE([protobuf-c-rpc has no prepare functions: not building the dispatch adapter])])])])
AM_CONDITIONAL([WITH_DISPATCH], [test "$have_dispatch" = yes])

# The GLib adapter needs GLib 2.36 (g_source_add_unix_fd).
AC_ARG_WITH([glib], AS_HELP_STRING([--without-glib], [do not build the GLib main-loop adapter]))
have_glib=no
AS_IF([test "x$with_glib" != xno],
      [PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.36], [have_glib=yes], [have_glib=no])])
AM_CONDITIONAL([WITH_GLIB], [test "$have_glib" = yes])

AC_ARG_WITH([openssl], AS_HELP_STRING([--without-openssl], [do not build the OpenSSL adapter]))
have_openssl=no
AS_IF([test "x$with_openssl" != xno],
      [PKG_CHECK_MODULES([OPENSSL], [openssl], [have_openssl=yes], [have_openssl=no])])
AM_CONDITIONAL([WITH_OPENSSL], [test "$have_openssl" = yes])

AC_ARG_WITH([gnutls], AS_HELP_STRING([--without-gnutls], [do not build the GnuTLS adapter]))
have_gnutls=no
AS_IF([test "x$with_gnutls" != xno],
      [PKG_CHECK_MODULES([GNUTLS], [gnutls], [have_gnutls=yes], [have_gnutls=no])])
AM_CONDITIONAL([WITH_GNUTLS], [test "$have_gnutls" = yes])

# Tests and examples are generated with protoc and protoc-gen-c.
AC_ARG_WITH([protoc], AS_HELP_STRING([--with-protoc=PATH], [protoc used to build tests]))
AC_ARG_WITH([protoc-gen-c], AS_HELP_STRING([--with-protoc-gen-c=PATH], [protoc-gen-c used to build tests]))
AS_IF([test "x$with_protoc" != x], [PROTOC="$with_protoc"], [AC_PATH_PROG([PROTOC], [protoc])])
AS_IF([test "x$with_protoc_gen_c" != x], [PROTOC_GEN_C="$with_protoc_gen_c"], [AC_PATH_PROG([PROTOC_GEN_C], [protoc-gen-c])])
AC_SUBST([PROTOC])
AC_SUBST([PROTOC_GEN_C])
AC_ARG_ENABLE([examples], AS_HELP_STRING([--disable-examples], [do not build the libuv examples]))
AM_CONDITIONAL([BUILD_TESTS], [test -n "$PROTOC" -a -n "$PROTOC_GEN_C"])
AM_CONDITIONAL([BUILD_ENDPOINTS_TEST],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_uv" = yes -a "$have_openssl" = yes])
AM_CONDITIONAL([BUILD_POLL_TESTS],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "x$with_poll" != xno])
AM_CONDITIONAL([BUILD_POLL_ENDPOINTS_TEST],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "x$with_poll" != xno -a "$have_openssl" = yes])
AM_CONDITIONAL([BUILD_DISPATCH_TESTS],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_dispatch" = yes])
AM_CONDITIONAL([BUILD_DISPATCH_ENDPOINTS_TEST],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_dispatch" = yes -a "$have_openssl" = yes])
AM_CONDITIONAL([BUILD_GLIB_TESTS],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_glib" = yes])
AM_CONDITIONAL([BUILD_GLIB_ENDPOINTS_TEST],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_glib" = yes -a "$have_openssl" = yes])
AM_CONDITIONAL([BUILD_GLIB_GNUTLS_ENDPOINTS_TEST],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_glib" = yes -a "$have_gnutls" = yes])
AM_CONDITIONAL([BUILD_GNUTLS_ENDPOINTS_TEST],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_gnutls" = yes -a "$have_uv" = yes])
AM_CONDITIONAL([BUILD_POLL_GNUTLS_ENDPOINTS_TEST],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_gnutls" = yes -a "x$with_poll" != xno])
AM_CONDITIONAL([BUILD_DISPATCH_GNUTLS_ENDPOINTS_TEST],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_gnutls" = yes -a "$have_dispatch" = yes])
AM_CONDITIONAL([BUILD_TLS_INTEROP_TEST], [test "$have_openssl" = yes -a "$have_gnutls" = yes])
AM_CONDITIONAL([BUILD_UV_TESTS],
               [test -n "$PROTOC" -a -n "$PROTOC_GEN_C" -a "$have_uv" = yes])
AM_CONDITIONAL([BUILD_EXAMPLES],
               [test "x$enable_examples" != xno -a "$have_uv" = yes -a -n "$PROTOC" -a -n "$PROTOC_GEN_C"])
AM_CONDITIONAL([EXAMPLES_OPENSSL], [test "$have_openssl" = yes])

AC_CONFIG_FILES([Makefile libprotobuf-c-grpc2.pc libprotobuf-c-grpc2-uv.pc libprotobuf-c-grpc2-poll.pc libprotobuf-c-grpc2-dispatch.pc libprotobuf-c-grpc2-glib.pc libprotobuf-c-grpc2-openssl.pc libprotobuf-c-grpc2-gnutls.pc])
AC_OUTPUT
